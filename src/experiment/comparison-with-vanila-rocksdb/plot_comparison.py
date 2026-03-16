#!/usr/bin/env python3
"""
Compare all experiment results in the result/ directory.
Parses parameter info from CSV filenames and plots comparison charts.
"""

import os
import re
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path

RESULT_DIR = Path(__file__).parent / "result"

FILENAME_PATTERN = re.compile(
    r"^(?P<workload>.+?)_(?P<engine>bitlsm|vanila)"
    r"_n(?P<n>\d+)_p(?P<p>\d+)_a(?P<a>\d+)"
    r"(?:_rho(?P<rho>[\d.]+))?"
    r"\.csv$"
)


def parse_filename(filename: str) -> dict | None:
    m = FILENAME_PATTERN.match(filename)
    if not m:
        return None
    d = m.groupdict()
    d["n"] = int(d["n"])
    d["p"] = int(d["p"])
    d["a"] = int(d["a"])
    if d["rho"] is not None:
        d["rho"] = float(d["rho"])
    return d


def build_label(params: dict) -> str:
    engine = "BitLSM" if params["engine"] == "bitlsm" else "Vanilla RocksDB"
    label = f"{engine} (a={params['a']}"
    if params["rho"] is not None:
        label += f", ρ={params['rho']}"
    label += ")"
    return label


def load_experiments():
    experiments = []
    for f in sorted(RESULT_DIR.iterdir()):
        if not f.is_file() or f.suffix != ".csv":
            continue
        params = parse_filename(f.name)
        if params is None:
            print(f"[WARN] Skipping unrecognized file: {f.name}")
            continue
        df = pd.read_csv(f)
        experiments.append({"params": params, "df": df, "filename": f.name})
    return experiments


def plot_throughput_over_time(experiments, output_dir: Path):
    """Plot throughput (records/sec) over records_written for each experiment."""
    fig, ax = plt.subplots(figsize=(12, 7))

    # Sort: vanila first, then bitlsm; within each group sort by a
    experiments_sorted = sorted(
        experiments, key=lambda e: (e["params"]["engine"] == "bitlsm", e["params"]["a"])
    )

    vanila_cmap = plt.cm.Blues
    bitlsm_cmap = plt.cm.Reds
    vanila_count = sum(1 for e in experiments_sorted if e["params"]["engine"] == "vanila")
    bitlsm_count = sum(1 for e in experiments_sorted if e["params"]["engine"] == "bitlsm")

    vi, bi = 0, 0
    for exp in experiments_sorted:
        df = exp["df"].copy()
        # Compute interval throughput (records per second)
        dt_sec = df["time_elapsed_ms"].diff() / 1000.0
        dr = df["records_written"].diff()
        throughput = dr / dt_sec
        records_m = df["records_written"] / 1e6

        label = build_label(exp["params"])
        if exp["params"]["engine"] == "vanila":
            color = vanila_cmap(0.4 + 0.5 * vi / max(vanila_count - 1, 1))
            vi += 1
            linestyle = "-"
        else:
            color = bitlsm_cmap(0.4 + 0.5 * bi / max(bitlsm_count - 1, 1))
            bi += 1
            linestyle = "--"

        ax.plot(records_m.iloc[1:], throughput.iloc[1:],
                label=label, color=color, linestyle=linestyle, linewidth=1.2)

    ax.set_xlabel("Records Written (M)", fontsize=12)
    ax.set_ylabel("Throughput (records/sec)", fontsize=12)
    ax.set_title("Write Throughput Over Time", fontsize=14)
    ax.legend(fontsize=8, loc="best")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(output_dir / "throughput_over_time.png", dpi=150)
    plt.close(fig)
    print(f"Saved: throughput_over_time.png")


def plot_total_time_by_attr(experiments, output_dir: Path):
    """Bar chart: total time for each (engine, a) combination."""
    data = []
    for exp in experiments:
        df = exp["df"]
        total_time_sec = df["time_elapsed_ms"].iloc[-1] / 1000.0
        data.append({
            "engine": exp["params"]["engine"],
            "a": exp["params"]["a"],
            "total_time_sec": total_time_sec,
            "label": build_label(exp["params"]),
        })

    data.sort(key=lambda x: (x["a"], x["engine"]))

    attrs = sorted(set(d["a"] for d in data))
    engines = ["vanila", "bitlsm"]
    engine_labels = {"vanila": "Vanilla RocksDB", "bitlsm": "BitLSM"}

    fig, ax = plt.subplots(figsize=(10, 6))
    bar_width = 0.35
    x = range(len(attrs))

    for i, engine in enumerate(engines):
        times = []
        for a in attrs:
            match = [d for d in data if d["engine"] == engine and d["a"] == a]
            times.append(match[0]["total_time_sec"] if match else 0)
        offset = (i - 0.5) * bar_width
        bars = ax.bar([xi + offset for xi in x], times, bar_width,
                      label=engine_labels[engine],
                      color=("steelblue" if engine == "vanila" else "indianred"))
        for bar, t in zip(bars, times):
            if t > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 1,
                        f"{t:.0f}s", ha="center", va="bottom", fontsize=8)

    ax.set_xlabel("Number of Attributes (a)", fontsize=12)
    ax.set_ylabel("Total Time (sec)", fontsize=12)
    ax.set_title("Total Write Time by Attribute Count", fontsize=14)
    ax.set_xticks(list(x))
    ax.set_xticklabels([str(a) for a in attrs])
    ax.legend(fontsize=10)
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(output_dir / "total_time_by_attr.png", dpi=150)
    plt.close(fig)
    print(f"Saved: total_time_by_attr.png")


def plot_avg_throughput_by_attr(experiments, output_dir: Path):
    """Bar chart: average throughput for each (engine, a) combination."""
    data = []
    for exp in experiments:
        df = exp["df"]
        total_records = df["records_written"].iloc[-1]
        total_time_sec = df["time_elapsed_ms"].iloc[-1] / 1000.0
        avg_throughput = total_records / total_time_sec
        data.append({
            "engine": exp["params"]["engine"],
            "a": exp["params"]["a"],
            "avg_throughput": avg_throughput,
        })

    attrs = sorted(set(d["a"] for d in data))
    engines = ["vanila", "bitlsm"]
    engine_labels = {"vanila": "Vanilla RocksDB", "bitlsm": "BitLSM"}

    fig, ax = plt.subplots(figsize=(10, 6))
    bar_width = 0.35
    x = range(len(attrs))

    for i, engine in enumerate(engines):
        vals = []
        for a in attrs:
            match = [d for d in data if d["engine"] == engine and d["a"] == a]
            vals.append(match[0]["avg_throughput"] if match else 0)
        offset = (i - 0.5) * bar_width
        bars = ax.bar([xi + offset for xi in x], [v / 1e6 for v in vals], bar_width,
                      label=engine_labels[engine],
                      color=("steelblue" if engine == "vanila" else "indianred"))
        for bar, v in zip(bars, vals):
            if v > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                        f"{v/1e6:.2f}M", ha="center", va="bottom", fontsize=8)

    ax.set_xlabel("Number of Attributes (a)", fontsize=12)
    ax.set_ylabel("Avg Throughput (M records/sec)", fontsize=12)
    ax.set_title("Average Write Throughput by Attribute Count", fontsize=14)
    ax.set_xticks(list(x))
    ax.set_xticklabels([str(a) for a in attrs])
    ax.legend(fontsize=10)
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(output_dir / "avg_throughput_by_attr.png", dpi=150)
    plt.close(fig)
    print(f"Saved: avg_throughput_by_attr.png")


def plot_slowdown_ratio(experiments, output_dir: Path):
    """Plot BitLSM slowdown ratio compared to Vanilla RocksDB, grouped by rho if present."""
    vanila_time = {}   # a -> total_time
    bitlsm_time = {}   # (a, rho) -> total_time

    for exp in experiments:
        a = exp["params"]["a"]
        engine = exp["params"]["engine"]
        total_time = exp["df"]["time_elapsed_ms"].iloc[-1]
        if engine == "vanila":
            vanila_time[a] = total_time
        else:
            rho = exp["params"]["rho"]
            bitlsm_time[(a, rho)] = total_time

    attrs = sorted(set(a for (a, _) in bitlsm_time if a in vanila_time))
    rhos = sorted(set(rho for (_, rho) in bitlsm_time if rho is not None))

    if not attrs:
        return

    fig, ax = plt.subplots(figsize=(10, 6))

    if rhos:
        n_rhos = len(rhos)
        bar_width = 0.8 / n_rhos
        for i, rho in enumerate(rhos):
            ratios = []
            for a in attrs:
                if (a, rho) in bitlsm_time and a in vanila_time:
                    ratios.append(bitlsm_time[(a, rho)] / vanila_time[a])
                else:
                    ratios.append(0)
            positions = [j + (i - (n_rhos - 1) / 2) * bar_width for j in range(len(attrs))]
            bars = ax.bar(positions, ratios, bar_width,
                          label=f"\u03c1={rho}", edgecolor="black", linewidth=0.5)
            for bar, r in zip(bars, ratios):
                if r > 0:
                    ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                            f"{r:.2f}x", ha="center", va="bottom", fontsize=8)
    else:
        ratios = []
        for a in attrs:
            if (a, None) in bitlsm_time and a in vanila_time:
                ratios.append(bitlsm_time[(a, None)] / vanila_time[a])
            else:
                ratios.append(0)
        bars = ax.bar(range(len(attrs)), ratios, color="mediumpurple",
                      edgecolor="black", linewidth=0.5)
        for bar, r in zip(bars, ratios):
            if r > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                        f"{r:.2f}x", ha="center", va="bottom", fontsize=10)

    ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=1, label="1x (no overhead)")
    ax.set_xlabel("Number of Attributes (a)", fontsize=12)
    ax.set_ylabel("Slowdown Ratio (BitLSM / Vanilla)", fontsize=12)
    ax.set_title("BitLSM Write Overhead vs Vanilla RocksDB", fontsize=14)
    ax.set_xticks(range(len(attrs)))
    ax.set_xticklabels([str(a) for a in attrs])
    ax.legend(fontsize=10)
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(output_dir / "slowdown_ratio.png", dpi=150)
    plt.close(fig)
    print(f"Saved: slowdown_ratio.png")


def plot_throughput_by_rho(experiments, output_dir: Path):
    """Plot BitLSM avg throughput vs rho for each attribute count, with Vanilla baselines."""
    bitlsm_data = []
    for exp in experiments:
        if exp["params"]["engine"] != "bitlsm" or exp["params"]["rho"] is None:
            continue
        df = exp["df"]
        total_records = df["records_written"].iloc[-1]
        total_time_sec = df["time_elapsed_ms"].iloc[-1] / 1000.0
        bitlsm_data.append({
            "a": exp["params"]["a"],
            "rho": exp["params"]["rho"],
            "avg_throughput": total_records / total_time_sec,
        })

    if not bitlsm_data:
        return

    vanila_throughput = {}
    for exp in experiments:
        if exp["params"]["engine"] != "vanila":
            continue
        df = exp["df"]
        a = exp["params"]["a"]
        total_records = df["records_written"].iloc[-1]
        total_time_sec = df["time_elapsed_ms"].iloc[-1] / 1000.0
        vanila_throughput[a] = total_records / total_time_sec

    attrs = sorted(set(d["a"] for d in bitlsm_data))
    rhos = sorted(set(d["rho"] for d in bitlsm_data))
    cmap = plt.cm.tab10

    fig, ax = plt.subplots(figsize=(10, 6))
    for i, a in enumerate(attrs):
        throughputs = []
        valid_rhos = []
        for rho in rhos:
            match = [d for d in bitlsm_data if d["a"] == a and d["rho"] == rho]
            if match:
                throughputs.append(match[0]["avg_throughput"] / 1e6)
                valid_rhos.append(rho)
        ax.plot(valid_rhos, throughputs, marker="o", label=f"BitLSM a={a}",
                color=cmap(i), linewidth=1.5)
        if a in vanila_throughput:
            ax.axhline(y=vanila_throughput[a] / 1e6, color=cmap(i),
                       linestyle="--", linewidth=0.8, alpha=0.5)

    ax.set_xlabel("\u03c1 (rho)", fontsize=12)
    ax.set_ylabel("Avg Throughput (M records/sec)", fontsize=12)
    ax.set_title("BitLSM Throughput by \u03c1 (dashed = Vanilla baseline)", fontsize=14)
    ax.legend(fontsize=9, loc="best")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(output_dir / "throughput_by_rho.png", dpi=150)
    plt.close(fig)
    print(f"Saved: throughput_by_rho.png")


def main():
    output_dir = RESULT_DIR / "plots"
    output_dir.mkdir(exist_ok=True)

    experiments = load_experiments()
    if not experiments:
        print("No experiment CSV files found in result/")
        return

    print(f"Loaded {len(experiments)} experiments:")
    for exp in experiments:
        print(f"  {exp['filename']}  -> {build_label(exp['params'])}")
    print()

    plot_throughput_over_time(experiments, output_dir)
    plot_total_time_by_attr(experiments, output_dir)
    plot_avg_throughput_by_attr(experiments, output_dir)
    plot_slowdown_ratio(experiments, output_dir)
    plot_throughput_by_rho(experiments, output_dir)

    print(f"\nAll plots saved to: {output_dir}")


if __name__ == "__main__":
    main()
