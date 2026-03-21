#!/usr/bin/env python3
"""
Plot seq_read_cont results: read performance by number of query attributes.
Reads directly from result/seq_read_cont.csv.
"""

import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np
from pathlib import Path

CSV_PATH = Path(__file__).parent.parent / "result" / "seq_read_cont.csv"
OUTPUT_DIR = Path(__file__).parent.parent / "result" / "plots"

ENGINE_LABELS = {
    "no-index": "No-Index",
    "bitlsm": "BitLSM",
    "si-lu/im": "SI-LU (IM)",
    "si-lu/pf": "SI-LU (PF)",
    "si-ck/im": "SI-CK (IM)",
    "si-ck/pf": "SI-CK (PF)",
}
ENGINE_COLORS = {
    "no-index": "steelblue",
    "bitlsm": "indianred",
    "si-lu/im": "mediumseagreen",
    "si-lu/pf": "limegreen",
    "si-ck/im": "darkorange",
    "si-ck/pf": "gold",
}
ENGINE_MARKERS = {
    "no-index": "s",
    "bitlsm": "o",
    "si-lu/im": "^",
    "si-lu/pf": "v",
    "si-ck/im": "D",
    "si-ck/pf": "d",
}


def load_data() -> pd.DataFrame:
    df = pd.read_csv(CSV_PATH)
    df["time_sec"] = df["time_elapsed_ms"] / 1000.0
    # Combine method + read_strategy into a unified method key
    df["method_key"] = df.apply(
        lambda r: f"{r['method']}/{r['read_strategy']}"
        if pd.notna(r.get("read_strategy")) and r.get("read_strategy") != ""
        else r["method"],
        axis=1,
    )
    return df


def plot_time_by_qa_per_selectivity(df: pd.DataFrame):
    """Line chart: read time vs query_attr_num, one subplot per selectivity."""
    selectivities = sorted(df["expected_selectivity"].unique(), reverse=True)
    methods = [m for m in ENGINE_LABELS if m in df["method_key"].unique()]
    n_sel = len(selectivities)
    cols = 3
    rows = (n_sel + cols - 1) // cols

    fig, axes = plt.subplots(rows, cols, figsize=(5 * cols, 4.5 * rows), squeeze=False)

    for idx, sel in enumerate(selectivities):
        ax = axes[idx // cols][idx % cols]
        subset = df[df["expected_selectivity"] == sel]

        for method in methods:
            mdata = subset[subset["method_key"] == method].sort_values("query_attr_num")
            if mdata.empty:
                continue
            ax.plot(
                mdata["query_attr_num"], mdata["time_sec"],
                marker=ENGINE_MARKERS[method], label=ENGINE_LABELS[method],
                color=ENGINE_COLORS[method], linewidth=1.8, markersize=7,
            )
            for _, row in mdata.iterrows():
                ax.annotate(
                    f"{row['time_sec']:.1f}s",
                    (row["query_attr_num"], row["time_sec"]),
                    textcoords="offset points", xytext=(0, 8),
                    ha="center", fontsize=7,
                )

        ax.set_title(f"Selectivity = {sel}", fontsize=11)
        ax.set_xlabel("# Query Attributes", fontsize=10)
        ax.set_ylabel("Read Time (sec)", fontsize=10)
        ax.set_xticks(sorted(df["query_attr_num"].unique()))
        ax.set_ylim(bottom=0)
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    # Hide unused subplots
    for idx in range(n_sel, rows * cols):
        axes[idx // cols][idx % cols].set_visible(False)

    fig.suptitle("Sequential Read Time by Query Attribute Count", fontsize=14, y=1.01)
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "seq_read_cont_time_by_qa.png", dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("Saved: seq_read_cont_time_by_qa.png")


def plot_speedup_by_qa(df: pd.DataFrame):
    """Bar chart: speedup ratio (no_index_time / method_time) by query_attr_num, one subplot per selectivity."""
    selectivities = sorted(df["expected_selectivity"].unique(), reverse=True)
    qa_nums = sorted(df["query_attr_num"].unique())
    indexed_methods = [m for m in ENGINE_LABELS if m != "no-index" and m in df["method_key"].unique()]

    no_index = df[df["method_key"] == "no-index"].set_index(["expected_selectivity", "query_attr_num"])

    n_sel = len(selectivities)
    cols = 3
    rows = (n_sel + cols - 1) // cols
    n_methods = len(indexed_methods)
    bar_width = 0.8 / n_methods
    x = np.arange(len(qa_nums))

    fig, axes = plt.subplots(rows, cols, figsize=(5 * cols, 4.5 * rows), squeeze=False)

    for idx, sel in enumerate(selectivities):
        ax = axes[idx // cols][idx % cols]

        for bar_idx, method in enumerate(indexed_methods):
            mdata = df[(df["method_key"] == method) & (df["expected_selectivity"] == sel)]
            mdata = mdata.set_index("query_attr_num")
            ratios = []
            for qa in qa_nums:
                try:
                    vt = no_index.loc[(sel, qa), "time_elapsed_ms"]
                    bt = mdata.loc[qa, "time_elapsed_ms"]
                    if hasattr(vt, 'iloc'):
                        vt = vt.iloc[0]
                    if hasattr(bt, 'iloc'):
                        bt = bt.iloc[0]
                    ratios.append(float(vt) / float(bt))
                except KeyError:
                    ratios.append(0)
            positions = x + (bar_idx - (n_methods - 1) / 2) * bar_width
            bars = ax.bar(
                positions, ratios, bar_width,
                label=ENGINE_LABELS[method], color=ENGINE_COLORS[method],
                edgecolor="black", linewidth=0.5,
            )
            for bar, r in zip(bars, ratios):
                if r > 0:
                    txt = f"{r:.1f}x" if r >= 1 else f"{r:.2f}x"
                    ax.text(
                        bar.get_x() + bar.get_width() / 2, bar.get_height(),
                        txt, ha="center", va="bottom", fontsize=6, rotation=90,
                    )

        ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=1, alpha=0.7)
        ax.set_title(f"Selectivity = {sel}", fontsize=11)
        ax.set_xlabel("# Query Attributes", fontsize=10)
        ax.set_ylabel("Speedup (No-Index / Method)", fontsize=10)
        ax.set_xticks(x)
        ax.set_xticklabels([str(q) for q in qa_nums])
        ax.legend(fontsize=8)
        ax.grid(True, axis="y", alpha=0.3)

    for idx in range(n_sel, rows * cols):
        axes[idx // cols][idx % cols].set_visible(False)

    fig.suptitle("Read Speedup by Query Attribute Count", fontsize=14, y=1.01)
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "seq_read_cont_speedup.png", dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("Saved: seq_read_cont_speedup.png")


def plot_time_by_selectivity_per_qa(df: pd.DataFrame):
    """Line chart: read time vs selectivity for each query_attr_num, comparing methods."""
    qa_nums = sorted(df["query_attr_num"].unique())
    methods = [m for m in ENGINE_LABELS if m in df["method_key"].unique()]
    n_qa = len(qa_nums)

    fig, axes = plt.subplots(1, n_qa, figsize=(5 * n_qa, 5), squeeze=False)

    for idx, qa in enumerate(qa_nums):
        ax = axes[0][idx]
        subset = df[df["query_attr_num"] == qa]

        for method in methods:
            mdata = subset[subset["method_key"] == method].sort_values("expected_selectivity")
            if mdata.empty:
                continue
            ax.plot(
                mdata["expected_selectivity"], mdata["time_sec"],
                marker=ENGINE_MARKERS[method], label=ENGINE_LABELS[method],
                color=ENGINE_COLORS[method], linewidth=1.8, markersize=7,
            )

        ax.set_xscale("log")
        ax.set_title(f"Query Attrs = {qa}", fontsize=12)
        ax.set_xlabel("Expected Selectivity", fontsize=10)
        ax.set_ylabel("Read Time (sec)", fontsize=10)
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)
        ax.invert_xaxis()

    fig.suptitle("Read Time by Selectivity (per Query Attribute Count)", fontsize=14, y=1.01)
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "seq_read_cont_time_by_sel.png", dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("Saved: seq_read_cont_time_by_sel.png")


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    df = load_data()
    print(f"Loaded {len(df)} rows from {CSV_PATH.name}")
    print(f"  Methods: {df['method_key'].unique().tolist()}")
    print(f"  Query attr nums: {sorted(df['query_attr_num'].unique())}")
    print(f"  Selectivities: {sorted(df['expected_selectivity'].unique())}")
    print()

    plot_time_by_qa_per_selectivity(df)
    plot_speedup_by_qa(df)
    plot_time_by_selectivity_per_qa(df)

    print(f"\nAll plots saved to: {OUTPUT_DIR}")


if __name__ == "__main__":
    main()