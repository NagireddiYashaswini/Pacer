#!/usr/bin/env python3
"""
Publication-Grade Visualizations for Measured-Speedup-Aware Hybrid CPU Scheduling.

PAPER INTEGRITY RULES:
  - All figures are built from real measurement data only.
  - np.random is NOT used anywhere in this file.
  - Fig3 (IPC vs MPKI scatter) requires real daemon_telemetry.csv rows.
    If the file is absent or contains only synthetic=true rows, the figure
    is skipped with an explicit error message rather than silently using
    fabricated data.
  - Rows tagged synthetic=true in any CSV are excluded from all figures.

Generates:
  results/fig1_edp_comparison.png   — EDP bar chart across policies
  results/fig2_telemetry_timeline.png — IPC/MPKI/migration timeline
  results/fig3_speedup_vs_mpki.png  — IPC vs MPKI scatter (real data only)
"""

import os
import sys
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# Publication-quality styling
plt.rcParams.update({
    "font.family":        "sans-serif",
    "font.sans-serif":    ["DejaVu Sans", "Arial", "Helvetica"],
    "font.size":          11,
    "axes.labelsize":     12,
    "axes.titlesize":     13,
    "xtick.labelsize":    10,
    "ytick.labelsize":    10,
    "legend.fontsize":    10,
    "figure.titlesize":   15,
    "figure.dpi":         300,
    "savefig.dpi":        300,
    "savefig.bbox":       "tight",
})

POLICY_COLORS = {
    "Default Linux (CFS)":       "#4A5568",
    "Static P-Core Pool":        "#E53E3E",
    "Static E-Core Pool":        "#3182CE",
    "Adaptive Hybrid Scheduler": "#38A169",
}


def _load_real_rows(csv_path: str, label: str) -> pd.DataFrame:
    """
    Load CSV, dropping rows tagged synthetic=true.
    Returns an empty DataFrame (not None) if none remain.
    Prints a warning if rows were dropped.
    """
    if not os.path.exists(csv_path):
        return pd.DataFrame()

    df = pd.read_csv(csv_path)
    if "synthetic" in df.columns:
        n_synth = df["synthetic"].astype(str).str.lower().isin(["true", "1"]).sum()
        if n_synth > 0:
            print(f"[WARN] {label}: dropping {n_synth} synthetic row(s) from figures.")
        df = df[~df["synthetic"].astype(str).str.lower().isin(["true", "1"])].copy()
    return df


# ---------------------------------------------------------------------------
# Fig 1: EDP bar chart
# ---------------------------------------------------------------------------

def plot_edp_comparison(
    eval_csv: str = "results/evaluation_results.csv",
    out_png: str = "results/fig1_edp_comparison.png",
):
    df = _load_real_rows(eval_csv, "evaluation_results.csv")
    if df.empty:
        print(f"[!] No real rows in {eval_csv} — skipping fig1.")
        return

    workloads = df["workload"].unique()
    policies  = df["policy"].unique()

    fig, ax = plt.subplots(figsize=(10, 5.5))
    x = range(len(workloads))
    width = 0.20

    for i, pol in enumerate(policies):
        subset = df[df["policy"] == pol]
        edp_vals = []
        for wl in workloads:
            match = subset[subset["workload"] == wl]["edp"]
            edp_vals.append(match.values[0] if not match.empty else 0.0)
        offset = (i - 1.5) * width
        ax.bar(
            [xi + offset for xi in x],
            edp_vals,
            width,
            label=pol,
            color=POLICY_COLORS.get(pol, "#888888"),
            edgecolor="black",
            linewidth=0.8,
        )

    ax.set_ylabel("Energy-Delay Product (EDP in Joules × Seconds)", fontweight="bold")
    ax.set_title(
        "Energy-Delay Product (EDP) Comparison Across CPU Scheduling Policies\n"
        "(Lower is Better)",
        fontweight="bold",
        pad=12,
    )
    ax.set_xticks(list(x))
    ax.set_xticklabels([w.replace(" (", "\n(") for w in workloads], fontweight="semibold")
    ax.legend(frameon=True, facecolor="#F7FAFC", edgecolor="#CBD5E0", loc="upper right")
    ax.grid(axis="y", linestyle="--", alpha=0.5)
    ax.set_axisbelow(True)

    plt.tight_layout()
    plt.savefig(out_png)
    plt.close()
    print(f"[+] Saved Figure 1 to {out_png}")


# ---------------------------------------------------------------------------
# Fig 2: Telemetry timeline
# ---------------------------------------------------------------------------

def plot_telemetry_timeline(
    telem_csv: str = "results/daemon_telemetry.csv",
    out_png: str = "results/fig2_telemetry_timeline.png",
):
    df = _load_real_rows(telem_csv, "daemon_telemetry.csv")
    if df.empty:
        print(f"[!] No real rows in {telem_csv} — skipping fig2.")
        return

    if "ipc" not in df.columns or "mpki" not in df.columns:
        print(f"[!] Required columns (ipc, mpki) missing in {telem_csv} — skipping fig2.")
        return

    worker_df = df[df["ipc"] > 0].copy()
    if worker_df.empty:
        worker_df = df.copy()

    # Use wall-clock ISO timestamp if available, else monotonic timestamp_sec
    if "timestamp_iso" in worker_df.columns:
        try:
            worker_df["_t"] = pd.to_datetime(worker_df["timestamp_iso"], utc=True)
            t0 = worker_df["_t"].iloc[0]
            worker_df["time_rel"] = (worker_df["_t"] - t0).dt.total_seconds()
        except Exception:
            worker_df["time_rel"] = worker_df["timestamp_sec"] - worker_df["timestamp_sec"].iloc[0]
    else:
        worker_df["time_rel"] = worker_df["timestamp_sec"] - worker_df["timestamp_sec"].iloc[0]

    fig, (ax1, ax2, ax3) = plt.subplots(
        3, 1, figsize=(11, 7.5), sharex=True,
        gridspec_kw={"height_ratios": [1.2, 1.2, 0.9]},
    )

    # Panel 1: IPC
    ax1.plot(worker_df["time_rel"], worker_df["ipc"],
             color="#2B6CB0", linewidth=2.2, label="Instructions Per Cycle (IPC)")
    ax1.axhline(1.2, color="#38A169", linestyle=":",
                label="Compute-Bound Threshold (IPC ≥ 1.2)")
    ax1.set_ylabel("IPC", fontweight="bold")
    ax1.set_title(
        "Real-Time Telemetry and Adaptive Core Steering Dynamics (bench_phase)",
        fontweight="bold",
    )
    ax1.grid(True, linestyle="--", alpha=0.5)
    ax1.legend(loc="upper right", framealpha=0.9)

    # Panel 2: MPKI
    ax2.plot(worker_df["time_rel"], worker_df["mpki"],
             color="#DD6B20", linewidth=2.2, label="LLC Misses Per 1K Instructions (MPKI)")
    ax2.axhline(15.0, color="#E53E3E", linestyle="--", linewidth=1.5,
                label="Memory-Stall Guard Threshold (MPKI = 15.0)")
    ax2.set_ylabel("MPKI", fontweight="bold")
    ax2.grid(True, linestyle="--", alpha=0.5)
    ax2.legend(loc="upper right", framealpha=0.9)

    # Panel 3: Core pool migration
    if "core_pool" in worker_df.columns:
        pool_num = [1 if str(p).startswith("P") else 0 for p in worker_df["core_pool"]]
        ax3.step(worker_df["time_rel"], pool_num, where="post",
                 color="#6B46C1", linewidth=2.5, label="Assigned Core Pool")
        ax3.set_yticks([0, 1])
        ax3.set_yticklabels(["E-Core Pool", "P-Core Pool"], fontweight="bold")
        ax3.set_ylim(-0.2, 1.2)
        ax3.legend(loc="upper right", framealpha=0.9)
    else:
        ax3.set_visible(False)

    ax3.set_xlabel("Elapsed Time (Seconds)", fontweight="bold")
    ax3.grid(True, linestyle="--", alpha=0.5)

    plt.tight_layout()
    plt.savefig(out_png)
    plt.close()
    print(f"[+] Saved Figure 2 to {out_png}")


# ---------------------------------------------------------------------------
# Fig 3: IPC vs MPKI scatter — REAL DATA ONLY
# ---------------------------------------------------------------------------

def plot_speedup_vs_mpki(
    telem_csv: str = "results/daemon_telemetry.csv",
    out_png:   str = "results/fig3_speedup_vs_mpki.png",
):
    """
    Scatter of IPC vs MPKI coloured by core_pool assignment.
    Data comes exclusively from real daemon telemetry.
    If no real data is available this function prints an error and returns
    without creating the figure — it does NOT synthesize data.
    """
    df = _load_real_rows(telem_csv, "daemon_telemetry.csv")
    if df.empty:
        print(
            f"[ERROR] Fig3 skipped: no real telemetry rows in {telem_csv}.\n"
            f"  Run the evaluation with real hardware (RAPL + PMU) to produce this figure.\n"
            f"  Do NOT fabricate IPC/MPKI data.",
            file=sys.stderr,
        )
        return

    if "ipc" not in df.columns or "mpki" not in df.columns:
        print(
            f"[ERROR] Fig3 skipped: 'ipc' or 'mpki' column missing in {telem_csv}.",
            file=sys.stderr,
        )
        return

    real = df[df["ipc"] > 0].copy()
    if real.empty:
        print(
            f"[ERROR] Fig3 skipped: all ipc values are 0 in {telem_csv} — "
            f"PMU data appears absent.",
            file=sys.stderr,
        )
        return

    fig, ax = plt.subplots(figsize=(9, 5.5))

    # Colour by core_pool if available; otherwise by a single colour
    if "core_pool" in real.columns:
        p_mask = real["core_pool"].astype(str).str.startswith("P")
        e_mask = ~p_mask
        ax.scatter(
            real.loc[p_mask, "mpki"], real.loc[p_mask, "ipc"],
            color="#E53E3E", s=50, alpha=0.75, edgecolors="black", linewidths=0.5,
            label="P-Core Pool",
        )
        ax.scatter(
            real.loc[e_mask, "mpki"], real.loc[e_mask, "ipc"],
            color="#3182CE", s=50, alpha=0.75, edgecolors="black", linewidths=0.5,
            label="E-Core Pool",
        )
    else:
        ax.scatter(real["mpki"], real["ipc"], s=50, alpha=0.75, edgecolors="black",
                   linewidths=0.5, label="Measured samples")

    # Decision-boundary reference lines (from daemon config, not invented data)
    ax.axvline(15.0, color="#E53E3E", linestyle="--", linewidth=1.5,
               label="MPKI threshold (memory-stall guard = 15)")
    ax.axhline(1.2,  color="#38A169", linestyle=":",  linewidth=1.5,
               label="IPC threshold (compute-boost = 1.2)")
    ax.axvspan(15.0, real["mpki"].max() * 1.05, color="#3182CE", alpha=0.08,
               label="Memory-Stall Zone → E-Core")
    ax.axvspan(0.0,  5.0,  color="#E53E3E", alpha=0.07,
               label="Compute-Boost Zone → P-Core")

    ax.set_xlabel("LLC Misses Per 1K Instructions (MPKI)", fontweight="bold")
    ax.set_ylabel("Instructions Per Cycle (IPC)", fontweight="bold")
    ax.set_title(
        "Microarchitectural Sensitivity Space: IPC vs. MPKI\n"
        "(real daemon telemetry, coloured by scheduled core pool)",
        fontweight="bold",
        pad=12,
    )
    ax.grid(True, linestyle="--", alpha=0.5)
    ax.legend(frameon=True, facecolor="#F7FAFC", edgecolor="#CBD5E0", loc="upper right",
              fontsize=9)

    plt.tight_layout()
    plt.savefig(out_png)
    plt.close()
    print(f"[+] Saved Figure 3 to {out_png}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    os.makedirs("results", exist_ok=True)
    plot_edp_comparison()
    plot_telemetry_timeline()
    plot_speedup_vs_mpki()
