"""Box-and-whisker plots of read query time, grouped by k (query attribute count)."""

import re
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

RESULT_DIR = Path(__file__).parent.parent / "result" / "read_seq_sweep_r100"
OUTPUT_DIR = RESULT_DIR / "plots"

FILENAME_PATTERN = re.compile(
    r"^read_seq_2025_sel(?P<sel>[\d.]+)_k(?P<k>\d+)_r\d+_"
    r"(?P<system>bitlsm|no-index|si-ck|si-lu)"
    r"(?:_(?P<params>.+?))?_read_log\.csv$"
)

VARIANT_ORDER = [
    "si-lu/im",
    "si-lu/pf",
    "si-ck/im",
    "si-ck/pf",
    "bitlsm/rho0.2",
    "bitlsm/rho0.1",
    "bitlsm/rho0.05",
]

VARIANT_LABELS = {
    "no-index": "No-Index",
    "bitlsm/rho0.05": "BitLSM\nρ=0.05",
    "bitlsm/rho0.1": "BitLSM\nρ=0.1",
    "bitlsm/rho0.2": "BitLSM\nρ=0.2",
    "si-lu/im": "SI-LU\n(IM)",
    "si-lu/pf": "SI-LU\n(PF)",
    "si-ck/im": "SI-CK\n(IM)",
    "si-ck/pf": "SI-CK\n(PF)",
}

VARIANT_COLORS = {
    "no-index": "steelblue",
    "bitlsm/rho0.05": "#cd5c5c",
    "bitlsm/rho0.1": "#e06666",
    "bitlsm/rho0.2": "#f08080",
    "si-lu/im": "#2e8b57",
    "si-lu/pf": "#66cdaa",
    "si-ck/im": "#9370db",
    "si-ck/pf": "#b19cd9",
}


def _build_variant_key(system, params):
    if system == "no-index":
        return "no-index"
    if system == "bitlsm":
        return f"bitlsm/{params}"
    if system in ("si-ck", "si-lu"):
        strategy = params.replace("strategy_", "") if params else "unknown"
        return f"{system}/{strategy}"
    return f"{system}/{params}"


def load_data():
    # {sel: {k: {variant: Series}}}
    data = {}
    for f in sorted(RESULT_DIR.iterdir()):
        m = FILENAME_PATTERN.match(f.name)
        if not m:
            continue
        sel = m.group("sel")
        k = int(m.group("k"))
        variant = _build_variant_key(m.group("system"), m.group("params"))
        df = pd.read_csv(f)
        data.setdefault(sel, {}).setdefault(k, {})[variant] = df["time_elapsed_ms"] / 1000
    return data


def plot_boxplots(data):
    sels = sorted(data.keys(), key=float, reverse=True)
    n_rows = len(sels)
    fig, axes = plt.subplots(n_rows, 4, figsize=(20, 10 / 3 * n_rows), squeeze=False)

    for row, sel in enumerate(sels):
        # Share y-axis within each row
        for col in range(1, 4):
            axes[row][col].sharey(axes[row][0])

        # Compute whisker-based y-limits for this row
        row_ymin, row_ymax = float("inf"), float("-inf")
        for k in [1, 2, 3, 4]:
            k_data = data[sel].get(k, {})
            for v in VARIANT_ORDER:
                if v not in k_data:
                    continue
                s = k_data[v]
                q1, q3 = s.quantile(0.25), s.quantile(0.75)
                iqr = q3 - q1
                wlo = s[s >= q1 - 1.5 * iqr].min()
                whi = s[s <= q3 + 1.5 * iqr].max()
                row_ymin = min(row_ymin, wlo)
                row_ymax = max(row_ymax, whi)
            # Include no-index mean line
            if "no-index" in (data[sel].get(k, {})):
                row_ymax = max(row_ymax, data[sel][k]["no-index"].mean())

        for col, k in enumerate([1, 2, 3, 4]):
            ax = axes[row][col]
            k_data = data[sel].get(k, {})
            present = [v for v in VARIANT_ORDER if v in k_data]

            if not present:
                ax.text(
                    0.5, 0.5, "No data available",
                    transform=ax.transAxes, ha="center", va="center",
                    fontsize=12, color="gray",
                )
                ax.set_title(f"k = {k}", fontsize=13)
                ax.set_xticks([])
                continue

            # Draw no-index mean as dashed line
            if "no-index" in k_data:
                no_idx_mean = k_data["no-index"].mean()
                ax.axhline(no_idx_mean, color=VARIANT_COLORS["no-index"],
                           linestyle="--", linewidth=1.2, alpha=0.8)

            bp = ax.boxplot(
                [k_data[v].values for v in present],
                patch_artist=True,
                widths=0.6,
                showfliers=True,
                flierprops=dict(marker=".", markersize=3, alpha=0.5),
                medianprops=dict(color="black", linewidth=1.5),
            )
            for patch, v in zip(bp["boxes"], present):
                patch.set_facecolor(VARIANT_COLORS[v])
                patch.set_alpha(0.8)

            ax.set_xticks([])
            ax.set_title(f"k = {k}", fontsize=13)
            ax.grid(True, axis="y", alpha=0.3)

        if row_ymin < row_ymax:
            margin = (row_ymax - row_ymin) * 0.05
            axes[row][0].set_ylim(row_ymin - margin, row_ymax + margin)

        axes[row][0].set_ylabel(f"sel={sel}\nRead Time (s)", fontsize=11)

    # Shared legend at top
    from matplotlib.patches import Patch
    from matplotlib.lines import Line2D
    legend_handles = [
        Line2D([0], [0], color=VARIANT_COLORS["no-index"], linestyle="--",
               linewidth=1.2, label="No-Index (mean)"),
    ] + [
        Patch(facecolor=VARIANT_COLORS[v], alpha=0.8,
              label=VARIANT_LABELS[v].replace("\n", " "))
        for v in VARIANT_ORDER
    ]
    fig.legend(
        handles=legend_handles, loc="upper center",
        ncol=len(VARIANT_ORDER) + 1, fontsize=9, frameon=False,
    )
    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(OUTPUT_DIR / "read_boxplot_by_k.pdf", dpi=300, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {OUTPUT_DIR / 'read_boxplot_by_k.pdf'}")


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    data = load_data()
    if not data:
        print("No read log CSV files found.")
        return
    plot_boxplots(data)


if __name__ == "__main__":
    main()
