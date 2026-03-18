#!/usr/bin/env python3
"""
Plot seq_read_cat_qa results: read performance by number of query attributes.
Reads directly from result/seq_read_cat_qa.csv.
"""

import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path

CSV_PATH = Path(__file__).parent.parent / "result" / "seq_read_cat_qa.csv"
OUTPUT_DIR = Path(__file__).parent.parent / "result" / "plots"

ENGINE_LABELS = {"no-index": "No-Index", "bitlsm": "BitLSM"}
ENGINE_COLORS = {"no-index": "steelblue", "bitlsm": "indianred"}
ENGINE_MARKERS = {"no-index": "s", "bitlsm": "o"}


def load_data() -> pd.DataFrame:
    df = pd.read_csv(CSV_PATH)
    df["time_sec"] = df["time_elapsed_ms"] / 1000.0
    return df


def plot_time_by_qa(df: pd.DataFrame):
    """Line chart: read time vs query_attr_num, comparing methods."""
    fig, ax = plt.subplots(figsize=(8, 5))

    for method in ["no-index", "bitlsm"]:
        mdata = df[df["method"] == method].sort_values("query_attr_num")
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

    ax.set_title("Sequential Read Time by Query Attribute Count (Categorical)", fontsize=13)
    ax.set_xlabel("# Query Attributes", fontsize=11)
    ax.set_ylabel("Read Time (sec)", fontsize=11)
    ax.set_xticks(sorted(df["query_attr_num"].unique()))
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "seq_read_cat_qa_time_by_qa.png", dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("Saved: seq_read_cat_qa_time_by_qa.png")


def plot_speedup_by_qa(df: pd.DataFrame):
    """Bar chart: BitLSM speedup ratio (no_index_time / bitlsm_time) by query_attr_num."""
    qa_nums = sorted(df["query_attr_num"].unique())

    no_index = df[df["method"] == "no-index"].set_index("query_attr_num")
    bitlsm = df[df["method"] == "bitlsm"].set_index("query_attr_num")

    ratios = []
    for qa in qa_nums:
        try:
            vt = no_index.loc[qa, "time_elapsed_ms"]
            bt = bitlsm.loc[qa, "time_elapsed_ms"]
            ratios.append(vt / bt)
        except KeyError:
            ratios.append(0)

    fig, ax = plt.subplots(figsize=(10, 5))
    x = np.arange(len(qa_nums))
    bars = ax.bar(x, ratios, color="mediumseagreen", edgecolor="black", linewidth=0.5)

    for bar, r in zip(bars, ratios):
        if r > 0:
            txt = f"{r:.1f}x" if r >= 1 else f"{r:.2f}x"
            ax.text(
                bar.get_x() + bar.get_width() / 2, bar.get_height(),
                txt, ha="center", va="bottom", fontsize=9,
            )

    ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=1, alpha=0.7)
    ax.set_xlabel("# Query Attributes", fontsize=12)
    ax.set_ylabel("Speedup (No-Index / BitLSM)", fontsize=12)
    ax.set_title("BitLSM Read Speedup by Query Attribute Count (Categorical)", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels([str(q) for q in qa_nums])
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "seq_read_cat_qa_speedup.png", dpi=150)
    plt.close(fig)
    print("Saved: seq_read_cat_qa_speedup.png")


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    df = load_data()
    print(f"Loaded {len(df)} rows from {CSV_PATH.name}")
    print(f"  Methods: {df['method'].unique().tolist()}")
    print(f"  Query attr nums: {sorted(df['query_attr_num'].unique())}")
    print()

    plot_time_by_qa(df)
    plot_speedup_by_qa(df)

    print(f"\nAll plots saved to: {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
