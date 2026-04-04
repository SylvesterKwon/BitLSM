"""Cumulative processing time under interleaved workload (write / query / total)."""

import re
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

RESULT_DIR = Path(__file__).parent.parent / "result"
OUTPUT_DIR = RESULT_DIR / "plots"

FILENAME_PATTERN = re.compile(
    r"^interleave_(?P<system>bitlsm|si-ck|si-lu)_(?P<params>.+)\.csv$"
)

VARIANT_ORDER = [
    "bitlsm/rho0.05",
    "bitlsm/rho0.1",
    # "bitlsm/rho0.2",
    "si-ck/im",
    "si-ck/pf",
    "si-lu/im",
    "si-lu/pf",
]

VARIANT_LABELS = {
    "bitlsm/rho0.05": r"BitLSM $\rho$=0.05",
    "bitlsm/rho0.1": r"BitLSM $\rho$=0.1",
    "bitlsm/rho0.2": r"BitLSM $\rho$=0.2",
    "si-ck/im": "SI-CK (IM)",
    "si-ck/pf": "SI-CK (PF)",
    "si-lu/im": "SI-LU (IM)",
    "si-lu/pf": "SI-LU (PF)",
}

VARIANT_STYLES = {
    "bitlsm/rho0.05": {"color": "#c0392b", "linestyle": "-"},
    "bitlsm/rho0.1":  {"color": "#c0392b", "linestyle": "--"},
    "bitlsm/rho0.2":  {"color": "#c0392b", "linestyle": ":"},
    "si-ck/im":        {"color": "#2471a3", "linestyle": "-"},
    "si-ck/pf":        {"color": "#2471a3", "linestyle": "--"},
    "si-lu/im":        {"color": "#1e8449", "linestyle": "-"},
    "si-lu/pf":        {"color": "#1e8449", "linestyle": "--"},
}

TOTAL_INTERLEAVE_RECORDS = 10_000_000


def _build_variant_key(system: str, params: str) -> str:
    if system == "bitlsm":
        return f"bitlsm/{params}"
    if system in ("si-ck", "si-lu"):
        strategy = params.replace("strategy_", "")
        return f"{system}/{strategy}"
    return f"{system}/{params}"


def _compute_cumulative_times(df: pd.DataFrame):
    """Return (data_pct, cum_write_s, cum_query_s) for the interleave phase.

    Write time: elapsed_us diff between consecutive PUT rows, minus any
    intervening QUERY latency so that only pure write time remains.
    Query time: sum of QUERY latency_us values.
    """
    first_query_idx = df[df["op_type"] == "QUERY"].index[0]
    inter = df.iloc[first_query_idx:].copy().reset_index(drop=True)
    start_rec = inter["records_written"].iloc[0] - 1000  # record count at prefill end

    ops = inter["op_type"].values
    elapsed = inter["elapsed_us"].values
    records = inter["records_written"].values

    data_pct = []
    cum_write = []
    cum_query = []
    total_write_us = 0
    total_query_us = 0

    # Use elapsed_us diffs for both write and query time.
    # Pattern is strictly alternating QUERY-PUT-QUERY-PUT, so:
    #   QUERY→PUT diff = write time for 1K PUTs
    #   PUT→QUERY diff = query time for 1K queries
    prev_elapsed = elapsed[0]  # first row is a QUERY

    for i in range(1, len(inter)):
        pct = (records[i] - start_rec) / TOTAL_INTERLEAVE_RECORDS * 100
        diff = elapsed[i] - prev_elapsed

        if ops[i] == "PUT":
            total_write_us += diff
        else:  # QUERY
            total_query_us += diff

        prev_elapsed = elapsed[i]
        data_pct.append(pct)
        cum_write.append(total_write_us / 1e6)
        cum_query.append(total_query_us / 1e6)

    return (
        np.array(data_pct),
        np.array(cum_write),
        np.array(cum_query),
    )


def load_data():
    """Load and process all interleave CSVs."""
    data = {}
    for f in sorted(RESULT_DIR.iterdir()):
        m = FILENAME_PATTERN.match(f.name)
        if not m:
            continue
        variant = _build_variant_key(m.group("system"), m.group("params"))
        df = pd.read_csv(f)
        if (df["op_type"] == "QUERY").sum() == 0:
            continue
        pct, cum_w, cum_q = _compute_cumulative_times(df)
        data[variant] = {"pct": pct, "write": cum_w, "query": cum_q}
    return data


def plot(data):
    plt.rcParams.update({
        "font.family": "serif",
        "font.size": 10,
        "axes.labelsize": 11,
        "axes.titlesize": 12,
        "legend.fontsize": 8,
        "xtick.labelsize": 9,
        "ytick.labelsize": 9,
        "figure.dpi": 300,
    })

    fig, axes = plt.subplots(1, 3, figsize=(14, 3.5), sharey=False, sharex=True)

    panels = [
        ("write", "(a) Cumulative Write Time"),
        ("query", "(b) Cumulative Query Time"),
        ("total", "(c) Cumulative Total Time"),
    ]

    for ax, (key, title) in zip(axes, panels):
        for variant in VARIANT_ORDER:
            if variant not in data:
                continue
            d = data[variant]
            style = VARIANT_STYLES[variant]
            if key == "total":
                y = d["write"] + d["query"]
            else:
                y = d[key]
            ax.plot(
                d["pct"], y,
                label=VARIANT_LABELS[variant],
                linewidth=1.5,
                **style,
            )

        ax.set_xlabel("Data Processed (%)")
        ax.set_ylabel("Time (s)")
        ax.set_title(title)
        ax.grid(True, alpha=0.3, linewidth=0.5)

    handles, labels = axes[-1].get_legend_handles_labels()
    fig.legend(
        handles, labels,
        loc="upper center",
        ncol=len(labels),
        framealpha=0.9,
        bbox_to_anchor=(0.5, 1.08),
    )

    fig.tight_layout()

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    out = OUTPUT_DIR / "interleave_cumulative.pdf"
    fig.savefig(out, dpi=300, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {out}")


def main():
    data = load_data()
    if not data:
        print("No interleave CSV files found.")
        return
    plot(data)


if __name__ == "__main__":
    main()
