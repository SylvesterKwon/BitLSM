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

RESULT_DIR = Path(__file__).parent.parent / "result"

FILENAME_PATTERN = re.compile(
    r"^(?P<workload>.+?)_(?P<index>bitlsm|no-index|si-lu|si-ck)"
    r"_n(?P<n>\d+)_schema_(?P<schema>.+?)_a(?P<a>\d+)"
    r"(?:_rho(?P<rho>[\d.]+))?"
    r"\.csv$"
)


def parse_filename(filename: str) -> dict | None:
    m = FILENAME_PATTERN.match(filename)
    if not m:
        return None
    d = m.groupdict()
    d["n"] = int(d["n"])
    d["a"] = int(d["a"])
    if d["rho"] is not None:
        d["rho"] = float(d["rho"])
    return d


INDEX_DISPLAY = {
    "no-index": "No-Index",
    "bitlsm": "BitLSM",
    "si-lu": "SI-LU",
    "si-ck": "SI-CK",
}

INDEX_STYLE = {
    "no-index":     {"cmap": plt.cm.Blues,    "linestyle": "-"},
    "bitlsm":       {"cmap": plt.cm.Reds,     "linestyle": "--"},
    "si-lu":         {"cmap": plt.cm.Greens,   "linestyle": "-."},
    "si-ck":         {"cmap": plt.cm.Purples,  "linestyle": ":"},
}

INDEX_ORDER = ["no-index", "bitlsm", "si-lu", "si-ck"]

INDEX_BAR_COLOR = {
    "no-index": "steelblue",
    "bitlsm": "indianred",
    "si-lu": "seagreen",
    "si-ck": "mediumpurple",
}


def build_label(params: dict) -> str:
    index_name = INDEX_DISPLAY.get(params["index"], params["index"])
    label = f"{index_name} (a={params['a']}"
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

    # Sort by index order, then by a
    index_rank = {e: i for i, e in enumerate(INDEX_ORDER)}
    experiments_sorted = sorted(
        experiments, key=lambda e: (index_rank.get(e["params"]["index"], 99), e["params"]["a"])
    )

    # Count per index for color gradient
    index_counts = {}
    for e in experiments_sorted:
        idx_name = e["params"]["index"]
        index_counts[idx_name] = index_counts.get(idx_name, 0) + 1
    index_counter = {}

    for exp in experiments_sorted:
        df = exp["df"].copy()
        # Compute interval throughput (records per second)
        dt_sec = df["time_elapsed_ms"].diff() / 1000.0
        dr = df["records_written"].diff()
        throughput = dr / dt_sec
        records_m = df["records_written"] / 1e6

        idx_name = exp["params"]["index"]
        style = INDEX_STYLE.get(idx_name, {"cmap": plt.cm.Greys, "linestyle": "-"})
        idx = index_counter.get(idx_name, 0)
        count = index_counts.get(idx_name, 1)
        color = style["cmap"](0.4 + 0.5 * idx / max(count - 1, 1))
        index_counter[idx_name] = idx + 1

        label = build_label(exp["params"])
        ax.plot(records_m.iloc[1:], throughput.iloc[1:],
                label=label, color=color, linestyle=style["linestyle"], linewidth=1.2)

    ax.set_xlabel("Records Written (M)", fontsize=12)
    ax.set_ylabel("Throughput (records/sec)", fontsize=12)
    ax.set_title("Write Throughput Over Time", fontsize=14)
    ax.legend(fontsize=8, loc="best")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(output_dir / "throughput_over_time.png", dpi=150)
    plt.close(fig)
    print(f"Saved: throughput_over_time.png")


def _build_bar_series(experiments):
    """Build (index, rho) series list and per-experiment data for bar charts."""
    data = []
    for exp in experiments:
        df = exp["df"]
        total_records = df["records_written"].iloc[-1]
        total_time_sec = df["time_elapsed_ms"].iloc[-1] / 1000.0
        data.append({
            "index": exp["params"]["index"],
            "a": exp["params"]["a"],
            "rho": exp["params"]["rho"],
            "total_time_sec": total_time_sec,
            "avg_throughput": total_records / total_time_sec,
        })

    series = []
    for idx_name in INDEX_ORDER:
        if not any(d["index"] == idx_name for d in data):
            continue
        rhos = sorted(set(d["rho"] for d in data if d["index"] == idx_name))
        cmap = INDEX_STYLE[idx_name]["cmap"]
        if rhos == [None]:
            series.append({"index": idx_name, "rho": None,
                           "label": INDEX_DISPLAY[idx_name],
                           "color": cmap(0.6)})
        else:
            n_rhos = len([r for r in rhos if r is not None])
            for j, rho in enumerate(r for r in rhos if r is not None):
                intensity = 0.35 + 0.5 * j / max(n_rhos - 1, 1)
                series.append({"index": idx_name, "rho": rho,
                               "label": f"{INDEX_DISPLAY[idx_name]} \u03c1={rho}",
                               "color": cmap(intensity)})

    return data, series


def plot_total_time_by_attr(experiments, output_dir: Path):
    """Bar chart: total time for each (index, rho, a) combination."""
    data, series = _build_bar_series(experiments)
    attrs = sorted(set(d["a"] for d in data))
    n_series = len(series)

    fig, ax = plt.subplots(figsize=(12, 6))
    bar_width = 0.8 / n_series
    x = range(len(attrs))

    for i, s in enumerate(series):
        times = []
        for a in attrs:
            match = [d for d in data if d["index"] == s["index"]
                     and d["a"] == a and d["rho"] == s["rho"]]
            times.append(match[0]["total_time_sec"] if match else 0)
        offset = (i - (n_series - 1) / 2) * bar_width
        bars = ax.bar([xi + offset for xi in x], times, bar_width,
                      label=s["label"], color=s["color"])
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
    """Bar chart: average throughput for each (index, rho, a) combination."""
    data, series = _build_bar_series(experiments)
    attrs = sorted(set(d["a"] for d in data))
    n_series = len(series)

    fig, ax = plt.subplots(figsize=(12, 6))
    bar_width = 0.8 / n_series
    x = range(len(attrs))

    for i, s in enumerate(series):
        vals = []
        for a in attrs:
            match = [d for d in data if d["index"] == s["index"]
                     and d["a"] == a and d["rho"] == s["rho"]]
            vals.append(match[0]["avg_throughput"] if match else 0)
        offset = (i - (n_series - 1) / 2) * bar_width
        bars = ax.bar([xi + offset for xi in x], [v / 1e6 for v in vals], bar_width,
                      label=s["label"], color=s["color"])
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
    """Plot slowdown ratio of all indexes compared to No-Index."""
    data, series = _build_bar_series(experiments)
    # Exclude no-index from series (it's the baseline)
    series = [s for s in series if s["index"] != "no-index"]

    no_index_time = {}
    for d in data:
        if d["index"] == "no-index":
            no_index_time[d["a"]] = d["total_time_sec"]

    if not series or not no_index_time:
        return

    attrs = sorted(no_index_time.keys())
    n_series = len(series)

    fig, ax = plt.subplots(figsize=(12, 6))
    bar_width = 0.8 / n_series

    for i, s in enumerate(series):
        ratios = []
        for a in attrs:
            match = [d for d in data if d["index"] == s["index"]
                     and d["a"] == a and d["rho"] == s["rho"]]
            if match and a in no_index_time:
                ratios.append(match[0]["total_time_sec"] / no_index_time[a])
            else:
                ratios.append(0)
        positions = [j + (i - (n_series - 1) / 2) * bar_width for j in range(len(attrs))]
        bars = ax.bar(positions, ratios, bar_width,
                      label=s["label"], color=s["color"],
                      edgecolor="black", linewidth=0.5)
        for bar, r in zip(bars, ratios):
            if r > 0:
                ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                        f"{r:.2f}x", ha="center", va="bottom", fontsize=7)

    ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=1, label="1x (no overhead)")
    ax.set_xlabel("Number of Attributes (a)", fontsize=12)
    ax.set_ylabel("Slowdown Ratio (Method / No-Index)", fontsize=12)
    ax.set_title("Write Overhead vs No-Index", fontsize=14)
    ax.set_xticks(range(len(attrs)))
    ax.set_xticklabels([str(a) for a in attrs])
    ax.legend(fontsize=8, loc="best")
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(output_dir / "slowdown_ratio.png", dpi=150)
    plt.close(fig)
    print(f"Saved: slowdown_ratio.png")


def plot_throughput_by_rho(experiments, output_dir: Path):
    """Plot BitLSM avg throughput vs rho for each attribute count, with No-Index baselines."""
    bitlsm_data = []
    for exp in experiments:
        if exp["params"]["index"] != "bitlsm" or exp["params"]["rho"] is None:
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

    no_index_throughput = {}
    for exp in experiments:
        if exp["params"]["index"] != "no-index":
            continue
        df = exp["df"]
        a = exp["params"]["a"]
        total_records = df["records_written"].iloc[-1]
        total_time_sec = df["time_elapsed_ms"].iloc[-1] / 1000.0
        no_index_throughput[a] = total_records / total_time_sec

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
        if a in no_index_throughput:
            ax.axhline(y=no_index_throughput[a] / 1e6, color=cmap(i),
                       linestyle="--", linewidth=0.8, alpha=0.5)

    ax.set_xlabel("\u03c1 (rho)", fontsize=12)
    ax.set_ylabel("Avg Throughput (M records/sec)", fontsize=12)
    ax.set_title("BitLSM Throughput by \u03c1 (dashed = No-Index baseline)", fontsize=14)
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
