"""Median query latency under active ingestion, binned every 1M records."""

import re
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

RESULT_DIR = Path(__file__).parent.parent / "result"
OUTPUT_DIR = RESULT_DIR / "plots"

FILENAME_PATTERN = re.compile(
    r"^interleave_(?P<system>bitlsm|si-ck|si-lu)_(?P<params>.+)\.csv$"
)

VARIANT_ORDER = [
    "bitlsm/rho0.05",
    "bitlsm/rho0.1",
    "bitlsm/rho0.2",
    "si-lu/im",
    "si-lu/pf",
    "si-ck/im",
    "si-ck/pf",
]

VARIANT_LABELS = {
    "bitlsm/rho0.05": "BitLSM ρ=0.05",
    "bitlsm/rho0.1": "BitLSM ρ=0.1",
    "bitlsm/rho0.2": "BitLSM ρ=0.2",
    "si-lu/im": "SI-LU (IM)",
    "si-lu/pf": "SI-LU (PF)",
    "si-ck/im": "SI-CK (IM)",
    "si-ck/pf": "SI-CK (PF)",
}

VARIANT_COLORS = {
    "bitlsm/rho0.05": "#cd5c5c",
    "bitlsm/rho0.1": "#e06666",
    "bitlsm/rho0.2": "#f08080",
    "si-lu/im": "#2e8b57",
    "si-lu/pf": "#66cdaa",
    "si-ck/im": "#9370db",
    "si-ck/pf": "#b19cd9",
}

VARIANT_MARKERS = {
    "bitlsm/rho0.05": "o",
    "bitlsm/rho0.1": "s",
    "bitlsm/rho0.2": "D",
    "si-lu/im": "^",
    "si-lu/pf": "v",
    "si-ck/im": "P",
    "si-ck/pf": "X",
}


def _build_variant_key(system, params):
    if system == "bitlsm":
        return f"bitlsm/{params}"
    if system in ("si-ck", "si-lu"):
        strategy = params.replace("strategy_", "")
        return f"{system}/{strategy}"
    return f"{system}/{params}"


def load_data():
    """Load QUERY rows from all interleave CSVs, return {variant: DataFrame}."""
    data = {}
    for f in sorted(RESULT_DIR.iterdir()):
        m = FILENAME_PATTERN.match(f.name)
        if not m:
            continue
        variant = _build_variant_key(m.group("system"), m.group("params"))
        df = pd.read_csv(f)
        queries = df[df["op_type"] == "QUERY"].copy()
        lo = queries["records_written"].min()
        hi = queries["records_written"].max()
        step = (hi - lo) / 10
        queries["bin"] = ((queries["records_written"] - lo) // step).clip(upper=9) * step + lo + step / 2
        queries["latency_ms"] = queries["latency_us"] / 1_000
        data[variant] = queries
    return data


def plot(data):
    fig, ax = plt.subplots(figsize=(10, 5))

    for variant in VARIANT_ORDER:
        if variant not in data:
            continue
        df = data[variant]
        medians = df.groupby("bin")["latency_ms"].median()
        x = medians.index / 1e6
        ax.plot(
            x, medians.values,
            label=VARIANT_LABELS[variant],
            color=VARIANT_COLORS[variant],
            marker=VARIANT_MARKERS[variant],
            markersize=5,
            linewidth=1.5,
        )

    ax.set_xlabel("Records in DB (millions)", fontsize=12)
    ax.set_ylabel("Median Query Latency (ms)", fontsize=12)
    ax.set_title("Query Latency under Active Ingestion (median per 1M bin)", fontsize=13)
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    out = OUTPUT_DIR / "interleave_median.pdf"
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
