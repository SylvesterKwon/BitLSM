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
"""

import argparse
import ctypes
import json
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
DB_PARAMS = ["n", "payload_size", "attr_num", "rho"]


def encode_params(params: dict) -> str:
    """Encode a {flag: value} dict into a path-safe string, e.g. n100000000_threads4."""
    return "_".join(f"{k}{fmt(v)}" for k, v in params.items())


def cartesian_combinations(params: dict) -> list:
    """Return list of {flag: single_value} dicts from {flag: [values]} input."""
    if not params:
        return [{}]
    keys = list(params.keys())
    values = [params[k] if isinstance(params[k], list) else [params[k]] for k in keys]
    return [dict(zip(keys, combo)) for combo in product(*values)]


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
    print("  [hw-reset] fstrim ...")
    subprocess.run(["fstrim", "-v", db_path_base], check=True)

    # 4. Wait for SSD controller to process TRIMs and reclaim SLC cache
    print("  [hw-reset] waiting 10s for SSD GC ...")
    time.sleep(10)
    print("  [hw-reset] done")


def cooldown_sleep(seconds: int):
    for remaining in range(seconds, 0, -1):
        print(f"\r  [cooldown] {remaining}s... ", end="", flush=True)
        time.sleep(1)
    print(f"\r  [cooldown] done          ")


def run(config_path: str, dry_run: bool, method_filter: list,
        cooldown: int, hw_reset: bool, keep_db: bool):
    with open(config_path) as f:
        config = json.load(f)

    exp_label    = config["exp_label"]
    exp_type     = config.get("exp_type", "write_seq")
    output_dir   = config["output_dir"]
    db_path_base = config["db_path_base"]
    common       = config.get("common_params", {})
    methods      = config["methods"]
    is_read_only = (exp_type == "read_seq")

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
        if dry_run:
            print("mode     : dry-run\n")
        else:
            print()

        global_idx = 0
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
                cmd     = build_command(binary, exp_label, exp_type, db_path, output_dir, combo)

                print(f"[{global_idx}/{total_runs}] [{name}] {' '.join(cmd)}")

                if not dry_run:
                    if is_read_only:
                        if not os.path.exists(db_path):
                            sys.exit(f"DB path does not exist (required for read_seq): {db_path}")
                    else:
                        os.makedirs(db_path, exist_ok=True)
                    os.makedirs(output_dir, exist_ok=True)
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
                        sys.stdout.write(data.decode("utf-8", errors="replace"))
                        sys.stdout.flush()
                    os.close(master_fd)
                    proc.wait()
                    if proc.returncode != 0:
                        sys.exit(f"Run failed (exit {proc.returncode}): {' '.join(cmd)}")
                    print()
                    if global_idx < total_runs:
                        if hw_reset:
                            reset_hardware(db_path_base, prev_db_path=db_path,
                                           keep_db=(keep_db or is_read_only))
                        if cooldown > 0:
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
    args = parser.parse_args()

    method_filter = args.methods.split(",") if args.methods else []
    run(args.config, dry_run=args.dry_run, method_filter=method_filter,
        cooldown=args.cooldown, hw_reset=args.hw_reset, keep_db=args.keep_db)


if __name__ == "__main__":
    main()
