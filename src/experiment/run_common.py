"""
Shared utilities for experiment runners (run.py, honk_run.py).

Provides: process execution, logging, hw-reset, cooldown, cartesian product,
CLI argument parsing, and daemon mode.
"""

import argparse
import ctypes
import os
import pty
import signal
import subprocess
import sys
import time
from datetime import datetime
from itertools import product


# ---------------------------------------------------------------------------
# Process management
# ---------------------------------------------------------------------------

def _set_pdeathsig():
    """Ask kernel to send SIGKILL to this process when its parent dies."""
    PR_SET_PDEATHSIG = 1
    ctypes.CDLL("libc.so.6").prctl(PR_SET_PDEATHSIG, signal.SIGKILL)


def run_process(cmd: list) -> tuple:
    """Execute a command with pty-based real-time output + capture.

    Returns (returncode, captured_output_str).
    """
    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        cmd, stdout=slave_fd, stderr=slave_fd,
        preexec_fn=_set_pdeathsig)
    os.close(slave_fd)
    captured = []
    while True:
        try:
            data = os.read(master_fd, 4096)
        except OSError:
            break
        if not data:
            break
        text = data.decode("utf-8", errors="replace")
        captured.append(text)
        sys.stdout.write(text)
        sys.stdout.flush()
    os.close(master_fd)
    proc.wait()
    return proc.returncode, "".join(captured)


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

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


def setup_logging(label: str):
    """Create log directory and tee stdout to a log file.

    Returns (log_file, log_path). Caller must close log_file and restore
    sys.stdout when done.
    """
    log_dir = "logs"
    os.makedirs(log_dir, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(log_dir, f"{timestamp}_{label}.log")
    try:
        log_file = open(log_path, "w")
        sys.stdout = TeeOutput(log_file)
    except PermissionError:
        log_file = None
        print(f"[warn] cannot write log to {log_path}, logging to stdout only")
    return log_file, log_path


def teardown_logging(log_file, log_path: str):
    """Restore stdout and close log file."""
    if log_file:
        print(f"Log saved to: {log_path}")
        log_file.close()
    sys.stdout = sys.__stdout__


# ---------------------------------------------------------------------------
# Parameter utilities
# ---------------------------------------------------------------------------

def fmt(val) -> str:
    """Format a parameter value to a clean string (no trailing zeros for floats)."""
    if isinstance(val, float):
        return f"{val:g}"
    return str(val)


def cartesian_combinations(params: dict) -> list:
    """Return list of {flag: single_value} dicts from {flag: [values]} input."""
    if not params:
        return [{}]
    keys = list(params.keys())
    values = [params[k] if isinstance(params[k], list) else [params[k]] for k in keys]
    return [dict(zip(keys, combo)) for combo in product(*values)]


# ---------------------------------------------------------------------------
# Hardware reset & cooldown
# ---------------------------------------------------------------------------

def reset_hardware(db_path_base: str, prev_db_path: str = None,
                   keep_db: bool = False):
    """Reset hardware state between runs: delete DB, sync, drop_caches, fstrim."""
    if not keep_db and prev_db_path and os.path.exists(prev_db_path):
        print(f"  [hw-reset] rm -rf {prev_db_path} ...")
        subprocess.run(["rm", "-rf", prev_db_path], check=True)

    print("  [hw-reset] sync + drop_caches ...")
    subprocess.run(["sync"], check=True)
    subprocess.run(["sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"], check=True)

    os.makedirs(db_path_base, exist_ok=True)
    print("  [hw-reset] fstrim ...")
    subprocess.run(["fstrim", "-v", db_path_base], check=True)

    print("  [hw-reset] waiting 10s for SSD GC ...")
    time.sleep(10)
    print("  [hw-reset] done")


def cooldown_sleep(seconds: int):
    """Sleep with countdown display."""
    for remaining in range(seconds, 0, -1):
        print(f"\r  [cooldown] {remaining}s... ", end="", flush=True)
        time.sleep(1)
    print(f"\r  [cooldown] done          ")


# ---------------------------------------------------------------------------
# CLI argument helpers
# ---------------------------------------------------------------------------

def add_common_args(parser: argparse.ArgumentParser):
    """Add CLI arguments shared by all experiment runners."""
    parser.add_argument("config", help="Path to params.json")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print commands without executing")
    parser.add_argument("--methods",
                        help="Comma-separated list of methods to run (default: all)")
    parser.add_argument("--cooldown", type=int, default=0, metavar="SECONDS",
                        help="Seconds to wait between runs (default: 0)")
    parser.add_argument("--hw-reset", action="store_true",
                        help="Reset hardware state between runs (sudo required)")
    parser.add_argument("--keep-db", action="store_true",
                        help="Do not delete DB directories between runs")
    parser.add_argument("--start-from", type=int, default=1, metavar="N",
                        help="Start from the N-th experiment (1-indexed, default: 1)")
    parser.add_argument("--daemon", action="store_true",
                        help="Run in background via nohup")


def maybe_run_as_daemon(args):
    """If --daemon is set, re-exec via nohup and exit. Otherwise return."""
    if not args.daemon:
        return
    child_argv = [a for a in sys.argv if a != "--daemon"]
    cmd = ["nohup", sys.executable] + child_argv
    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        preexec_fn=os.setpgrp)
    print(f"Background PID: {proc.pid}")
    print(f"- Check daemon status: ps -p {proc.pid}")
    print(f"- Terminate daemon: kill {proc.pid}")
    sys.exit(0)


def parse_method_filter(args) -> list:
    """Parse --methods flag into a list (empty = all)."""
    return args.methods.split(",") if args.methods else []
