#!/usr/bin/env python3
"""
Publication-Grade Visualizations for Measured-Speedup-Aware Hybrid CPU Scheduling.

PAPER INTEGRITY RULES:
  - All figures are built strictly from real measurement data.
  - np.random is NOT used anywhere in this file.
  - Rows tagged synthetic=true, warmup=true, session_type=smoke,
    session_valid=false, or row_valid=false are excluded from all figures.
  - Summary EDP bars use results/analysis_summary.csv (from harness/analyze.py)
    with 95% bootstrap confidence intervals; INSUFFICIENT cells are omitted.

Generates:
  results/fig1_edp_comparison.png    — EDP comparison with 95% bootstrap CIs
  results/fig2_telemetry_timeline.png — Real-time IPC, MPKI, and core pool timeline
  results/fig3_speedup_vs_mpki.png   — IPC vs MPKI scatter (real data only)
  results/fig4_probe_overhead.png    — Probe status & daemon CPU time breakdown
"""

import argparse
import glob
import os
import sys
from typing import Optional

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

plt.rcParams.update({
    "font.family":      "sans-serif",
    "font.sans-serif":  ["DejaVu Sans", "Arial", "Helvetica"],
    "font.size":        11,
    "axes.labelsize":   12,
    "axes.titlesize":   13,
    "xtick.labelsize":  10,
    "ytick.labelsize":  10,
    "legend.fontsize":  10,
    "figure.titlesize": 15,
    "figure.dpi":       300,
    "savefig.dpi":      300,
    "savefig.bbox":     "tight",
})

POLICY_COLORS = {
    "default-linux":    "#4A5568",
    "static-P":         "#E53E3E",
    "static-E":         "#3182CE",
    "oracle-static":    "#805AD5",
    "pie-like":         "#D69E2E",
    "pie-like-topdown": "#B7791F",
    "hass-like":        "#319795",
    "pacer-mpki":       "#DD6B20",
    "pacer-speedup":    "#38A169",
}


def _is_true(series: pd.Series) -> pd.Series:
    return series.astype(str).str.strip().str.lower().isin(["true", "1"])


def _load_real_rows(csv_path: str, label: str) -> pd.DataFrame:
    """
    Load CSV, dropping synthetic, warmup, smoke, and invalid rows.
    Returns an empty DataFrame if none remain.
    """
    if not os.path.exists(csv_path):
        return pd.DataFrame()
    df = pd.read_csv(csv_path)
    if df.empty:
        return df

    drop_mask = pd.Series(False, index=df.index)
    if "synthetic" in df.columns:
        drop_mask |= _is_true(df["synthetic"])
    if "warmup" in df.columns:
        drop_mask |= _is_true(df["warmup"])
    if "session_type" in df.columns:
        drop_mask |= df["session_type"].astype(str).str.strip().str.lower() == "smoke"
    if "session_valid" in df.columns:
        drop_mask |= ~_is_true(df["session_valid"])
    if "row_valid" in df.columns:
        drop_mask |= ~_is_true(df["row_valid"])

    n_drop = int(drop_mask.sum())
    if n_drop > 0:
        print(f"[WARN] {label}: excluded {n_drop} non-evaluation/invalid row(s).")
    return df[~drop_mask].copy()


def plot_edp_comparison(
    summary_csv: str = "results/analysis_summary.csv",
    out_png: str = "results/fig1_edp_comparison.png",
) -> bool:
    if not os.path.exists(summary_csv):
        print(f"[!] {summary_csv} not found (run harness/analyze.py first). Skipping fig1.")
        return False

    df = pd.read_csv(summary_csv)
    if "status" in df.columns:
        df = df[df["status"] == "OK"].copy()
    if df.empty:
        print(f"[!] No OK rows in {summary_csv} — skipping fig1.")
        return False

    workloads = list(df["workload_variant"].unique())
    policies = list(df["policy"].unique())

    fig, ax = plt.subplots(figsize=(max(10, len(workloads) * 1.8), 5.5))
    x = list(range(len(workloads)))
    width = 0.8 / max(1, len(policies))

    for i, pol in enumerate(policies):
        sub = df[df["policy"] == pol]
        means, err_lo, err_hi = [], [], []
        for wl in workloads:
            m = sub[sub["workload_variant"] == wl]
            if m.empty:
                means.append(0.0)
                err_lo.append(0.0)
                err_hi.append(0.0)
            else:
                row = m.iloc[0]
                mu = float(row["mean"])
                lo = max(0.0, mu - float(row["ci_lo"]))
                hi = max(0.0, float(row["ci_hi"]) - mu)
                means.append(mu)
                err_lo.append(lo)
                err_hi.append(hi)

        offset = (i - (len(policies) - 1) / 2.0) * width
        ax.bar(
            [xi + offset for xi in x],
            means,
            width,
            yerr=[err_lo, err_hi],
            capsize=2.5,
            label=pol,
            color=POLICY_COLORS.get(pol, "#888888"),
            edgecolor="black",
            linewidth=0.7,
        )

    ax.set_ylabel("Energy-Delay Product (EDP, J·s)", fontweight="bold")
    ax.set_title(
        "Energy-Delay Product (EDP) Across Policies (95% Bootstrap CI, Lower is Better)",
        fontweight="bold",
        pad=12,
    )
    ax.set_xticks(x)
    ax.set_xticklabels([w.replace("_", "\n") for w in workloads], fontsize=9)
    ax.legend(frameon=True, facecolor="#F7FAFC", edgecolor="#CBD5E0", ncol=2, fontsize=8.5)
    ax.grid(axis="y", linestyle="--", alpha=0.5)
    ax.set_axisbelow(True)

    plt.tight_layout()
    plt.savefig(out_png)
    plt.close()
    print(f"[+] Saved Figure 1 to {out_png}")
    return True


def plot_telemetry_timeline(
    telem_csv: str = "results/daemon_telemetry.csv",
    out_png: str = "results/fig2_telemetry_timeline.png",
) -> bool:
    df = _load_real_rows(telem_csv, os.path.basename(telem_csv))
    if df.empty or "ipc" not in df.columns or "mpki" not in df.columns:
        print(f"[!] No usable telemetry rows in {telem_csv} — skipping fig2.")
        return False

    worker_df = df[df["ipc"] > 0].copy()
    if worker_df.empty:
        worker_df = df.copy()

    if "timestamp_iso" in worker_df.columns:
        try:
            t_dt = pd.to_datetime(worker_df["timestamp_iso"], utc=True)
            worker_df["time_rel"] = (t_dt - t_dt.iloc[0]).dt.total_seconds()
        except Exception:
            worker_df["time_rel"] = worker_df["timestamp_sec"] - worker_df["timestamp_sec"].iloc[0]
    else:
        worker_df["time_rel"] = worker_df["timestamp_sec"] - worker_df["timestamp_sec"].iloc[0]

    fig, (ax1, ax2, ax3) = plt.subplots(
        3, 1, figsize=(11, 7.5), sharex=True,
        gridspec_kw={"height_ratios": [1.2, 1.2, 0.9]},
    )

    ax1.plot(worker_df["time_rel"], worker_df["ipc"], color="#2B6CB0", linewidth=2.0, label="IPC")
    ax1.axhline(1.2, color="#38A169", linestyle=":", label="Compute Threshold (IPC ≥ 1.2)")
    ax1.set_ylabel("IPC", fontweight="bold")
    ax1.set_title("Real-Time Daemon Telemetry and Core Pool Steering", fontweight="bold")
    ax1.grid(True, linestyle="--", alpha=0.5)
    ax1.legend(loc="upper right", framealpha=0.9)

    ax2.plot(worker_df["time_rel"], worker_df["mpki"], color="#DD6B20", linewidth=2.0, label="MPKI")
    ax2.axhline(15.0, color="#E53E3E", linestyle="--", linewidth=1.4, label="Memory-Stall Threshold (15.0)")
    ax2.set_ylabel("MPKI", fontweight="bold")
    ax2.grid(True, linestyle="--", alpha=0.5)
    ax2.legend(loc="upper right", framealpha=0.9)

    if "core_pool" in worker_df.columns:
        pool_num = [1 if str(p).startswith("P") else 0 for p in worker_df["core_pool"]]
        ax3.step(worker_df["time_rel"], pool_num, where="post", color="#6B46C1", linewidth=2.2, label="Pool")
        ax3.set_yticks([0, 1])
        ax3.set_yticklabels(["E-Core Pool", "P-Core Pool"], fontweight="bold")
        ax3.set_ylim(-0.2, 1.2)
        ax3.legend(loc="upper right", framealpha=0.9)
    else:
        ax3.set_visible(False)

    ax3.set_xlabel("Elapsed Time (s)", fontweight="bold")
    ax3.grid(True, linestyle="--", alpha=0.5)

    plt.tight_layout()
    plt.savefig(out_png)
    plt.close()
    print(f"[+] Saved Figure 2 to {out_png}")
    return True


def plot_speedup_vs_mpki(
    telem_csv: str = "results/daemon_telemetry.csv",
    out_png: str = "results/fig3_speedup_vs_mpki.png",
) -> bool:
    df = _load_real_rows(telem_csv, os.path.basename(telem_csv))
    if df.empty or "ipc" not in df.columns or "mpki" not in df.columns:
        print(f"[ERROR] Fig3 skipped: no real telemetry rows in {telem_csv}.", file=sys.stderr)
        return False

    real = df[df["ipc"] > 0].copy()
    if real.empty:
        print(f"[ERROR] Fig3 skipped: all ipc values are 0 in {telem_csv}.", file=sys.stderr)
        return False

    fig, ax = plt.subplots(figsize=(9, 5.5))
    if "core_pool" in real.columns:
        p_mask = real["core_pool"].astype(str).str.startswith("P")
        ax.scatter(real.loc[p_mask, "mpki"], real.loc[p_mask, "ipc"],
                   color="#E53E3E", s=50, alpha=0.75, edgecolors="black", linewidths=0.5, label="P-Core Pool")
        ax.scatter(real.loc[~p_mask, "mpki"], real.loc[~p_mask, "ipc"],
                   color="#3182CE", s=50, alpha=0.75, edgecolors="black", linewidths=0.5, label="E-Core Pool")
    else:
        ax.scatter(real["mpki"], real["ipc"], s=50, alpha=0.75, edgecolors="black", linewidths=0.5)

    ax.axvline(15.0, color="#E53E3E", linestyle="--", linewidth=1.4, label="MPKI = 15")
    ax.axhline(1.2, color="#38A169", linestyle=":", linewidth=1.4, label="IPC = 1.2")
    ax.set_xlabel("LLC Misses Per 1K Instructions (MPKI)", fontweight="bold")
    ax.set_ylabel("Instructions Per Cycle (IPC)", fontweight="bold")
    ax.set_title("Microarchitectural Sensitivity Space: IPC vs. MPKI (Real Telemetry)", fontweight="bold", pad=12)
    ax.grid(True, linestyle="--", alpha=0.5)
    ax.legend(frameon=True, facecolor="#F7FAFC", edgecolor="#CBD5E0", loc="upper right", fontsize=9)

    plt.tight_layout()
    plt.savefig(out_png)
    plt.close()
    print(f"[+] Saved Figure 3 to {out_png}")
    return True


def plot_probe_overhead(
    probe_glob: str = "results/raw/*/probe_logs/*.csv",
    out_png: str = "results/fig4_probe_overhead.png",
) -> bool:
    paths = sorted(glob.glob(probe_glob))
    if not paths:
        print(f"[!] No probe logs found at {probe_glob} — skipping fig4.")
        return False
    df = pd.concat([pd.read_csv(p) for p in paths], ignore_index=True)
    if df.empty or "status" not in df.columns:
        print("[!] Empty probe logs — skipping fig4.")
        return False

    counts = df["status"].value_counts()
    fig, ax = plt.subplots(figsize=(6.5, 4.5))
    colors = {"valid": "#38A169", "skipped": "#D69E2E", "invalidated": "#E53E3E"}
    bars = ax.bar(
        counts.index.tolist(),
        counts.values.tolist(),
        color=[colors.get(s, "#718096") for s in counts.index],
        edgecolor="black",
        linewidth=0.8,
    )
    ax.set_ylabel("Probe Count", fontweight="bold")
    ax.set_title("Speedup Probe Outcomes Across Evaluation Sessions", fontweight="bold")
    ax.grid(axis="y", linestyle="--", alpha=0.5)
    ax.set_axisbelow(True)
    for b in bars:
        h = b.get_height()
        ax.annotate(f"{int(h)}", (b.get_x() + b.get_width() / 2.0, h),
                    ha="center", va="bottom", fontsize=10, fontweight="bold")

    plt.tight_layout()
    plt.savefig(out_png)
    plt.close()
    print(f"[+] Saved Figure 4 to {out_png}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate publication figures from real Pacer results.")
    parser.add_argument("--summary-csv", default="results/analysis_summary.csv")
    parser.add_argument("--telemetry-csv", default="results/daemon_telemetry.csv")
    parser.add_argument("--probe-glob", default="results/raw/*/probe_logs/*.csv")
    parser.add_argument("--out-dir", default="results")
    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    plot_edp_comparison(args.summary_csv, os.path.join(args.out_dir, "fig1_edp_comparison.png"))
    plot_telemetry_timeline(args.telemetry_csv, os.path.join(args.out_dir, "fig2_telemetry_timeline.png"))
    plot_speedup_vs_mpki(args.telemetry_csv, os.path.join(args.out_dir, "fig3_speedup_vs_mpki.png"))
    plot_probe_overhead(args.probe_glob, os.path.join(args.out_dir, "fig4_probe_overhead.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
