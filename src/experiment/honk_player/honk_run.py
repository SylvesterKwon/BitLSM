#!/usr/bin/env python3
"""
Honk Player experiment sweep runner.

Reads a params.json and executes honk_player with all method × param
combinations against a taxi TSV workload.

Usage:
    python3 src/experiment/honk_player/honk_run.py <path/to/params.json> [options]

Options:
    --dry-run             Print commands without executing.
    --methods m1,m2       Run only the specified methods (comma-separated).
    --cooldown SECONDS    Wait between runs (default: 0).
    --hw-reset            Reset hardware state between runs (sudo required).
    --keep-db             Do not delete DB directories between runs.
    --start-from N        Start from the N-th experiment (1-indexed, default: 1).
    --daemon              Run in background via nohup.
    --output-dir DIR      Result CSV output directory (default: derived from config).

params.json format:
    {
      "db_path_base": "/scratch/honk",
      "workload": "path/to/taxi_workload.tsv",
      "methods": [
        {"name": "bitlsm",   "params": {"rho": [0.05, 0.1, 0.2]}},
        {"name": "no-index",  "params": {}},
        {"name": "si-ck",     "params": {"read_strategy": ["im", "pf"]}},
        {"name": "si-lu",     "params": {"read_strategy": ["im", "pf"]}}
      ]
    }
"""

import argparse
import json
import os
import sys
import tempfile

# Allow importing run_common from the parent experiment directory
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from run_common import (
    add_common_args,
    cartesian_combinations,
    cooldown_sleep,
    fmt,
    maybe_run_as_daemon,
    parse_method_filter,
    reset_hardware,
    run_process,
    setup_logging,
    teardown_logging,
)

BINARY = "build/bin/honk_player"


def encode_method_params(params: dict) -> str:
    """Encode method params into a path-safe string, e.g. 'rho0.1' or 'read_strategy_im'."""
    if not params:
        return "default"
    return "_".join(f"{k}{fmt(v)}" for k, v in params.items())


def workload_stem(path: str) -> str:
    """Extract filename stem from a workload path."""
    return os.path.splitext(os.path.basename(path))[0]


def build_honk_command(method_name: str, workload: str, db_path: str,
                       output_dir: str, combo: dict,
                       common_params: dict = None) -> list:
    """Build honk_player command line."""
    cmd = [BINARY,
           "--binding", method_name,
           "--workload", workload,
           "--db_path", db_path,
           "--output_dir", output_dir]
    if common_params:
        for key, val in common_params.items():
            cmd += [f"--{key}", fmt(val)]
    for key, val in combo.items():
        cmd += [f"--{key}", fmt(val)]
    return cmd


def run(config_path: str, dry_run: bool, method_filter: list,
        cooldown: int, hw_reset: bool, keep_db: bool,
        start_from: int = 1, output_dir_override: str = None,
        warmup: bool = False):
    with open(config_path) as f:
        config = json.load(f)

    exp_label     = os.path.splitext(os.path.basename(config_path))[0]
    db_path_base  = config["db_path_base"]
    raw_workload  = config["workload"]
    workloads     = raw_workload if isinstance(raw_workload, list) else [raw_workload]
    methods       = config["methods"]
    common_params = config.get("common_params", {})

    # Output directory: CLI override > config > default
    if output_dir_override:
        output_dir = output_dir_override
    elif "output_dir" in config:
        output_dir = config["output_dir"]
    else:
        output_dir = os.path.join(os.path.dirname(os.path.abspath(config_path)), "result")

    # Setup logging
    log_file, log_path = setup_logging(f"honk_{exp_label}")

    try:
        if method_filter:
            methods = [m for m in methods if m["name"] in method_filter]
            if not methods:
                sys.exit(f"No methods matched: {method_filter}")

        method_combos = sum(
            len(cartesian_combinations(m.get("params", {})))
            for m in methods
        )
        total_runs = len(workloads) * method_combos
        print(f"config   : {config_path}")
        print(f"label    : {exp_label}")
        print(f"axes     : {len(workloads)} workload(s) × {method_combos} method-combo(s)")
        for w in workloads:
            print(f"           - {workload_stem(w)}")
        print(f"output   : {output_dir}")
        print(f"total    : {total_runs} run(s)")
        print(f"cooldown : {cooldown}s between runs")
        print(f"hw-reset : {'on (sudo)' if hw_reset else 'off'}")
        print(f"keep-db  : {'on' if keep_db else 'off'}")
        print(f"warmup   : {'on (per-db)' if warmup else 'off'}")
        if dry_run:
            print("mode     : dry-run\n")
        else:
            print()

        global_idx = 0
        warmed_up_dbs = set()

        if hw_reset and not dry_run:
            print("[pre-run] initial hw-reset ...")
            reset_hardware(db_path_base)
            if cooldown > 0:
                cooldown_sleep(cooldown)

        for workload in workloads:
            for method in methods:
                name          = method["name"]
                method_params = method.get("params", {})
                combos        = cartesian_combinations(method_params)

                for combo in combos:
                    global_idx += 1
                    if global_idx < start_from:
                        print(f"[{global_idx}/{total_runs}] [{name}] SKIP (--start-from {start_from})")
                        continue

                    wl_stem = workload_stem(workload)
                    db_path = f"{db_path_base}/{name}/{encode_method_params(combo)}"
                    cmd = build_honk_command(name, workload, db_path, output_dir, combo, common_params)

                    print(f"[{global_idx}/{total_runs}] [{wl_stem}][{name}] {' '.join(cmd)}")

                    if not dry_run:
                        os.makedirs(db_path, exist_ok=True)
                        os.makedirs(output_dir, exist_ok=True)

                        # Warmup: single read per db_path to populate OS page cache
                        if warmup and db_path not in warmed_up_dbs:
                            print(f"  [warmup] warming up {db_path} ...")
                            with open(workload) as wf:
                                first_line = wf.readline()
                            with tempfile.NamedTemporaryFile(
                                mode="w", suffix=".tsv", delete=False
                            ) as tmp:
                                tmp.write(first_line)
                                tmp_path = tmp.name
                            try:
                                warmup_cmd = build_honk_command(
                                    name, tmp_path, db_path, output_dir,
                                    combo, common_params)
                                rc, _ = run_process(warmup_cmd)
                                if rc != 0:
                                    sys.exit(f"Warmup failed (exit {rc}): {' '.join(warmup_cmd)}")
                            finally:
                                os.unlink(tmp_path)
                            warmed_up_dbs.add(db_path)
                            print(f"  [warmup] done")

                        rc, _ = run_process(cmd)
                        if rc != 0:
                            sys.exit(f"Run failed (exit {rc}): {' '.join(cmd)}")
                        print()

                        if hw_reset:
                            reset_hardware(db_path_base, prev_db_path=db_path,
                                           keep_db=keep_db)
                        if cooldown > 0 and global_idx < total_runs:
                            cooldown_sleep(cooldown)

    finally:
        teardown_logging(log_file, log_path)


def main():
    parser = argparse.ArgumentParser(
        description="Run honk_player with all method × param combinations"
    )
    add_common_args(parser)
    parser.add_argument("--warmup", action="store_true",
                        help="Run one warmup query per DB before measuring "
                             "(populates OS page cache)")
    parser.add_argument("--output-dir",
                        help="Result CSV output directory (overrides config)")
    args = parser.parse_args()

    maybe_run_as_daemon(args)

    method_filter = parse_method_filter(args)
    run(args.config, dry_run=args.dry_run, method_filter=method_filter,
        cooldown=args.cooldown, hw_reset=args.hw_reset, keep_db=args.keep_db,
        start_from=args.start_from, output_dir_override=args.output_dir,
        warmup=args.warmup)


if __name__ == "__main__":
    main()
