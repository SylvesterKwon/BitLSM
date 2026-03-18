#!/usr/bin/env python3
"""
Global experiment sweep runner.

Reads a params.json from an experiment directory and executes all parameter
combinations (cartesian product of common_params × method-specific params).

Usage:
    python3 src/experiment/run.py <path/to/params.json> [options]

Options:
    --dry-run             Print commands without executing.
    --methods m1,m2       Run only the specified methods (comma-separated).
    --cooldown SECONDS    Wait between runs (default: 0).
    --hw-reset            Reset hardware state between runs (run script with sudo).
                          Runs: sync, drop_caches, fstrim on db_path_base.
    --keep-db             Do not delete DB directories between runs.
    --daemon              Run in background via nohup (logs to logs/ as usual).
"""

import argparse
import csv
import ctypes
import json
import math
import os
import pty
import signal
import subprocess
import sys
import time
from itertools import product
from datetime import datetime


def _set_pdeathsig():
    """Ask kernel to send SIGKILL to this process when its parent dies."""
    PR_SET_PDEATHSIG = 1
    ctypes.CDLL("libc.so.6").prctl(PR_SET_PDEATHSIG, signal.SIGKILL)


class TeeOutput:
    """Write output to both stdout and a log file."""
    def __init__(self, log_file):
        self.log_file = log_file
        self.stdout = sys.stdout

    def write(self, data):
        self.stdout.write(data)
        self.log_file.write(data)
        self.log_file.flush()

    def flush(self):
        self.stdout.flush()
        self.log_file.flush()

    def isatty(self):
        return self.stdout.isatty()


def fmt(val) -> str:
    """Format a parameter value to a clean string (no trailing zeros for floats)."""
    if isinstance(val, float):
        return f"{val:g}"
    return str(val)


# Parameters that define a DB identity — used to build deterministic db_path.
# Keys not present in a given combination are silently skipped.
DB_PARAMS = ["n", "schema", "rho"]


def _path_safe(key: str, val) -> str:
    """Format a single key-value pair for path encoding.

    For 'schema', extracts the filename stem (e.g. 'schema/default_a16.json' -> 'default_a16').
    """
    if key == "schema":
        return f"{key}_{os.path.splitext(os.path.basename(str(val)))[0]}"
    return f"{key}{fmt(val)}"


def encode_params(params: dict) -> str:
    """Encode a {flag: value} dict into a path-safe string, e.g. n100000000_schema_default_a16."""
    return "_".join(_path_safe(k, v) for k, v in params.items())


def cartesian_combinations(params: dict) -> list:
    """Return list of {flag: single_value} dicts from {flag: [values]} input."""
    if not params:
        return [{}]
    keys = list(params.keys())
    values = [params[k] if isinstance(params[k], list) else [params[k]] for k in keys]
    return [dict(zip(keys, combo)) for combo in product(*values)]


def resolve_expected_selectivity(combo: dict) -> dict:
    """Convert expected_selectivity (overall) to per-attr selectivity for the binary.

    If 'expected_selectivity' is present, compute per-attr selectivity as
    expected_selectivity^(1/k) where k = number of query attributes.
    Returns a new combo dict with 'selectivity' replacing 'expected_selectivity'.
    """
    if "expected_selectivity" not in combo:
        return combo
    resolved = dict(combo)
    expected_sel = resolved.pop("expected_selectivity")
    k = len(str(resolved.get("query_attr_indices", "0")).split(","))
    per_attr_sel = expected_sel ** (1.0 / k)
    resolved["selectivity"] = per_attr_sel
    return resolved


def build_command(binary: str, exp_label: str, exp_type: str,
                  db_path: str, output_dir: str, combo: dict) -> list:
    cmd = [binary,
           "--exp_label", exp_label,
           "--exp_type",  exp_type,
           "--db_path",  db_path,
           "--output_dir", output_dir]
    for key, val in combo.items():
        prefix = "-" if len(key) == 1 else "--"
        cmd += [f"{prefix}{key}", fmt(val)]
    return cmd


def reset_hardware(db_path_base: str, prev_db_path: str = None, keep_db: bool = False):
    """Reset hardware state between runs."""
    # 1. Delete previous DB so its blocks become trimmable
    if not keep_db and prev_db_path and os.path.exists(prev_db_path):
        print(f"  [hw-reset] rm -rf {prev_db_path} ...")
        subprocess.run(["rm", "-rf", prev_db_path], check=True)

    # 2. Sync pending writes to disk
    print("  [hw-reset] sync + drop_caches ...")
    subprocess.run(["sync"], check=True)
    subprocess.run(["sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"], check=True)

    # 3. TRIM freed blocks — SSD controller can now reclaim them
    #    fstrim needs the mount point to exist
    os.makedirs(db_path_base, exist_ok=True)
    print("  [hw-reset] fstrim ...")
    subprocess.run(["fstrim", "-v", db_path_base], check=True)

    # 4. Wait for SSD controller to process TRIMs and reclaim SLC cache
    print("  [hw-reset] waiting 10s for SSD GC ...")
    time.sleep(10)
    print("  [hw-reset] done")


def _schema_stem(schema_path: str) -> str:
    """Extract stem from schema path: 'schema/default_a16.json' -> 'default_a16'."""
    return os.path.splitext(os.path.basename(schema_path))[0]


RESULT_COLUMNS = ["time_elapsed_ms", "records_matched", "selectivity_actual"]


def _parse_result_line(output: str) -> dict | None:
    """Parse 'RESULT:val1,val2,val3' from binary stdout."""
    for line in output.splitlines():
        if line.startswith("RESULT:"):
            values = line.split("RESULT:", 1)[1].strip().split(",")
            return dict(zip(RESULT_COLUMNS, values))
    return None


def _build_master_fieldnames(common: dict, methods: list) -> list:
    """Build the fixed CSV fieldnames from the union of all methods' params."""
    # Collect all combo keys across every method
    all_keys = set()
    for m in methods:
        merged = {**common, **m.get("params", {})}
        for k in merged:
            if k == "query_attr_indices":
                all_keys.add("query_attr_num")
            else:
                all_keys.add(k)
    return ["method"] + sorted(all_keys) + RESULT_COLUMNS


def _append_to_master_csv(master_path: str, method: str, combo: dict,
                           result_data: dict, fieldnames: list):
    """Append a row to the master CSV with combo params + result values."""
    row = {"method": method}
    for k, v in combo.items():
        if k == "schema":
            row[k] = _schema_stem(str(v))
        elif k == "query_attr_indices":
            row["query_attr_num"] = len(str(v).split(","))
        else:
            row[k] = fmt(v)
    row.update(result_data)

    # Append to master (write header if file is new/empty, or recreate if schema changed)
    write_header = not os.path.exists(master_path) or os.path.getsize(master_path) == 0
    if not write_header:
        with open(master_path, "r", newline="") as f:
            existing = csv.DictReader(f).fieldnames
        if existing != fieldnames:
            print(f"  [warn] CSV header mismatch — recreating {master_path}")
            os.remove(master_path)
            write_header = True
    with open(master_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, restval="")
        if write_header:
            writer.writeheader()
        writer.writerow(row)


def cooldown_sleep(seconds: int):
    for remaining in range(seconds, 0, -1):
        print(f"\r  [cooldown] {remaining}s... ", end="", flush=True)
        time.sleep(1)
    print(f"\r  [cooldown] done          ")


def run(config_path: str, dry_run: bool, method_filter: list,
        cooldown: int, hw_reset: bool, keep_db: bool, warmup: bool = False):
    with open(config_path) as f:
        config = json.load(f)

    exp_label    = os.path.splitext(os.path.basename(config_path))[0]
    exp_type     = config.get("exp_type", "write_seq")
    db_path_base = config["db_path_base"]
    common       = config.get("common_params", {})
    methods      = config["methods"]
    is_read_only = (exp_type == "read_seq")

    # Derive exp_dir from config path: exp_set's parent directory
    # e.g. src/experiment/benchmark/exp_set/seq_write.json
    #   -> src/experiment/benchmark
    exp_dir    = os.path.dirname(os.path.dirname(os.path.abspath(config_path)))
    output_dir = os.path.join(exp_dir, "result")
    schema_dir = os.path.join(exp_dir, "schema")

    # Resolve schema filenames to full paths
    if "schema" in common:
        schemas = common["schema"] if isinstance(common["schema"], list) else [common["schema"]]
        common["schema"] = [os.path.join(schema_dir, s) for s in schemas]

    # Setup logging
    log_dir = "logs"
    os.makedirs(log_dir, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file_path = os.path.join(log_dir, f"{timestamp}_{exp_label}.log")
    log_file = open(log_file_path, "w")
    sys.stdout = TeeOutput(log_file)

    try:
        if method_filter:
            methods = [m for m in methods if m["name"] in method_filter]
            if not methods:
                sys.exit(f"No methods matched: {method_filter}")

        master_fieldnames = _build_master_fieldnames(common, methods)
        total_runs = sum(
            len(cartesian_combinations({**common, **m.get("params", {})}))
            for m in methods
        )
        print(f"config   : {config_path}")
        print(f"label    : {exp_label}")
        print(f"type     : {exp_type}")
        print(f"total    : {total_runs} run(s) across {len(methods)} method(s)")
        print(f"cooldown : {cooldown}s between runs")
        print(f"hw-reset : {'on (sudo)' if hw_reset else 'off'}")
        print(f"keep-db  : {'on' if keep_db else 'off'}")
        print(f"warmup   : {'on (per-db)' if warmup else 'off'}")
        if dry_run:
            print("mode     : dry-run\n")
        else:
            print()

        warmed_up_dbs = set()
        global_idx = 0

        if hw_reset and not dry_run:
            print("[pre-run] initial hw-reset ...")
            reset_hardware(db_path_base)
            if cooldown > 0:
                cooldown_sleep(cooldown)

        for method in methods:
            name         = method["name"]
            binary       = method["binary"]
            method_params = method.get("params", {})

            all_params = {**common, **method_params}
            combos     = cartesian_combinations(all_params)

            for combo in combos:
                global_idx += 1
                db_combo = {k: v for k, v in combo.items() if k in DB_PARAMS}
                db_path = f"{db_path_base}/{name}/{encode_params(db_combo)}"
                cmd_combo = resolve_expected_selectivity(combo)
                cmd     = build_command(binary, exp_label, exp_type, db_path, output_dir, cmd_combo)

                print(f"[{global_idx}/{total_runs}] [{name}] {' '.join(cmd)}")

                if not dry_run:
                    if is_read_only:
                        if not os.path.exists(db_path):
                            sys.exit(f"DB path does not exist (required for read_seq): {db_path}")
                        # Warmup: run once per db_path to populate OS page cache
                        if warmup and db_path not in warmed_up_dbs:
                            print(f"  [warmup] warming up {db_path} ...")
                            master_fd, slave_fd = pty.openpty()
                            proc = subprocess.Popen(
                                cmd, stdout=slave_fd, stderr=slave_fd,
                                preexec_fn=_set_pdeathsig)
                            os.close(slave_fd)
                            while True:
                                try:
                                    data = os.read(master_fd, 4096)
                                except OSError:
                                    break
                                if not data:
                                    break
                            os.close(master_fd)
                            proc.wait()
                            if proc.returncode != 0:
                                sys.exit(f"Warmup failed (exit {proc.returncode}): {' '.join(cmd)}")
                            warmed_up_dbs.add(db_path)
                            print(f"  [warmup] done")
                    else:
                        os.makedirs(db_path, exist_ok=True)
                    os.makedirs(output_dir, exist_ok=True)
                    master_fd, slave_fd = pty.openpty()
                    proc = subprocess.Popen(
                        cmd, stdout=slave_fd, stderr=slave_fd,
                        preexec_fn=_set_pdeathsig)
                    os.close(slave_fd)
                    captured_output = []
                    while True:
                        try:
                            data = os.read(master_fd, 4096)
                        except OSError:
                            break
                        if not data:
                            break
                        text = data.decode("utf-8", errors="replace")
                        captured_output.append(text)
                        sys.stdout.write(text)
                        sys.stdout.flush()
                    os.close(master_fd)
                    proc.wait()
                    if proc.returncode != 0:
                        sys.exit(f"Run failed (exit {proc.returncode}): {' '.join(cmd)}")
                    print()

                    # Append to master CSV for read experiments
                    if is_read_only:
                        full_output = "".join(captured_output)
                        result_data = _parse_result_line(full_output)
                        if result_data:
                            master_csv = os.path.join(output_dir, f"{exp_label}.csv")
                            _append_to_master_csv(master_csv, name, combo, result_data, master_fieldnames)
                    if hw_reset:
                        reset_hardware(db_path_base, prev_db_path=db_path,
                                       keep_db=(keep_db or is_read_only))
                    if cooldown > 0 and global_idx < total_runs:
                        cooldown_sleep(cooldown)

        print(f"Log saved to: {log_file_path}")
    finally:
        log_file.close()
        sys.stdout = sys.__stdout__




def main():
    parser = argparse.ArgumentParser(
        description="Run all parameter combinations defined in an experiment's params.json"
    )
    parser.add_argument("config", help="Path to params.json")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print commands without executing")
    parser.add_argument("--methods",
                        help="Comma-separated list of methods to run (default: all)")
    parser.add_argument("--cooldown", type=int, default=0, metavar="SECONDS",
                        help="Seconds to wait between runs (default: 0)")
    parser.add_argument("--hw-reset", action="store_true",
                        help="Reset hardware state between runs: "
                             "sync + drop_caches + fstrim (run script with sudo)")
    parser.add_argument("--keep-db", action="store_true",
                        help="Do not delete DB directories between runs")
    parser.add_argument("--warmup", action="store_true",
                        help="Run one warmup query per DB before measuring (read_seq only, "
                             "populates OS page cache)")
    parser.add_argument("--daemon", action="store_true",
                        help="Run in background via nohup (logs to logs/ as usual)")
    args = parser.parse_args()

    if args.daemon:
        # Rebuild argv without --daemon
        child_argv = [a for a in sys.argv if a != "--daemon"]
        cmd = ["nohup", sys.executable] + child_argv
        proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            preexec_fn=os.setpgrp)
        print(f"Background PID: {proc.pid}")
        print(f"Check logs: tail -f logs/<timestamp>_*.log")
        sys.exit(0)

    method_filter = args.methods.split(",") if args.methods else []
    run(args.config, dry_run=args.dry_run, method_filter=method_filter,
        cooldown=args.cooldown, hw_reset=args.hw_reset, keep_db=args.keep_db,
        warmup=args.warmup)


if __name__ == "__main__":
    main()
