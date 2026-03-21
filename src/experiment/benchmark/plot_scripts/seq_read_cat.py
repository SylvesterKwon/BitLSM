#!/usr/bin/env python3
"""
Plot seq_read_cat results: read performance by number of query attributes.
Reads directly from result/seq_read_cat.csv.
Generates one figure per plot type with subplots side-by-side per schema (cardinality).
"""

import re
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path

CSV_PATH = Path(__file__).parent.parent / "result" / "seq_read_cat.csv"
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


MIN_EXPECTED_MATCHES = 1


def _parse_cardinality(schema: str) -> int:
    """Extract cardinality from schema name like 'default_a16_c1000'."""
    m = re.search(r"_c(\d+)", schema)
    return int(m.group(1)) if m else 0


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
    # Filter out combinations where expected matches ≈ 0
    def _has_enough_matches(row):
        c = _parse_cardinality(row["schema"])
        if c == 0:
            return True
        expected = row["n"] * (1.0 / c) ** row["query_attr_num"]
        return expected >= MIN_EXPECTED_MATCHES

    df = df[df.apply(_has_enough_matches, axis=1)].reset_index(drop=True)
    return df


def plot_time_by_qa(df: pd.DataFrame, ax: plt.Axes, schema: str, show_legend: bool):
    """Line chart on ax: read time vs query_attr_num, comparing methods."""
    methods = [m for m in ENGINE_LABELS if m in df["method_key"].unique()]

    for method in methods:
        mdata = df[df["method_key"] == method].sort_values("query_attr_num")
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

    ax.set_title(schema, fontsize=12)
    ax.set_xlabel("# Query Attributes", fontsize=10)
    ax.set_ylabel("Read Time (sec)", fontsize=10)
    ax.set_xticks(sorted(df["query_attr_num"].unique()))
    ax.set_ylim(bottom=0)
    if show_legend:
        ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)


def plot_speedup_by_qa(df: pd.DataFrame, ax: plt.Axes, schema: str, show_legend: bool):
    """Bar chart on ax: speedup ratio (no_index_time / method_time) by query_attr_num."""
    qa_nums = sorted(df["query_attr_num"].unique())
    indexed_methods = [m for m in ENGINE_LABELS if m != "no-index" and m in df["method_key"].unique()]

    no_index = df[df["method_key"] == "no-index"].set_index("query_attr_num")

    n_methods = len(indexed_methods)
    bar_width = 0.8 / max(n_methods, 1)
    x = np.arange(len(qa_nums))

    for bar_idx, method in enumerate(indexed_methods):
        mdata = df[df["method_key"] == method].set_index("query_attr_num")
        ratios = []
        for qa in qa_nums:
            try:
                vt = no_index.loc[qa, "time_elapsed_ms"]
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
                    txt, ha="center", va="bottom", fontsize=7, rotation=90,
                )

    ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=1, alpha=0.7)
    ax.set_title(schema, fontsize=12)
    ax.set_xlabel("# Query Attributes", fontsize=10)
    ax.set_ylabel("Speedup (No-Index / Method)", fontsize=10)
    ax.set_xticks(x)
    ax.set_xticklabels([str(q) for q in qa_nums])
    if show_legend:
        ax.legend(fontsize=8)
    ax.grid(True, axis="y", alpha=0.3)


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    df = load_data()
    schemas = sorted(df["schema"].unique())
    n = len(schemas)
    print(f"Loaded {len(df)} rows from {CSV_PATH.name}")
    print(f"  Methods: {df['method_key'].unique().tolist()}")
    print(f"  Schemas: {schemas}")
    print(f"  Query attr nums: {sorted(df['query_attr_num'].unique())}")
    print()

    # Time plot: one row, n columns
    fig1, axes1 = plt.subplots(1, n, figsize=(7 * n, 5), sharey=True)
    if n == 1:
        axes1 = [axes1]
    for i, schema in enumerate(schemas):
        sdf = df[df["schema"] == schema]
        plot_time_by_qa(sdf, axes1[i], schema, show_legend=(i == n - 1))
    fig1.suptitle("Sequential Read Time by Query Attribute Count (Categorical)", fontsize=14, y=1.02)
    fig1.tight_layout()
    fig1.savefig(OUTPUT_DIR / "seq_read_cat_time_by_qa.png", dpi=150, bbox_inches="tight")
    plt.close(fig1)
    print("Saved: seq_read_cat_time_by_qa.png")

    # Speedup plot: one row, n columns
    fig2, axes2 = plt.subplots(1, n, figsize=(7 * n, 5), sharey=True)
    if n == 1:
        axes2 = [axes2]
    for i, schema in enumerate(schemas):
        sdf = df[df["schema"] == schema]
        plot_speedup_by_qa(sdf, axes2[i], schema, show_legend=(i == n - 1))
    fig2.suptitle("Read Speedup by Query Attribute Count (Categorical)", fontsize=14, y=1.02)
    fig2.tight_layout()
    fig2.savefig(OUTPUT_DIR / "seq_read_cat_speedup.png", dpi=150, bbox_inches="tight")
    plt.close(fig2)
    print("Saved: seq_read_cat_speedup.png")

    print(f"\nAll plots saved to: {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
