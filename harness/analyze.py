#!/usr/bin/env python3
"""
Statistical Analysis & Table Generator for Pacer (Prompt 4, Item 7).

Reads ONLY results/raw/*/runs.csv.
Excludes: warmup=true, session_type=smoke, synthetic=true, session_valid=false.
Prints exact counts of excluded rows.

Computes per (policy, workload_variant):
  - mean, std, median, 95% bootstrap CI (seeded, >= 10000 resamples)
  - prints INSUFFICIENT if valid_trials < 10

Computes paired comparisons vs baselines (default-linux, static-P, static-E,
oracle-static, pie-like):
  - Paired Wilcoxon signed-rank test
  - Matched-pairs rank-biserial correlation r
  - Percent change with 95% bootstrap CI
  - Holm-Bonferroni correction across the entire comparison family
  - Plain wins / ties / losses counts
Outputs CSV and LaTeX tables.
"""

import argparse
import glob
import math
import os
import random
from typing import Dict, List, Optional, Tuple

import numpy as np
import pandas as pd
from scipy.stats import wilcoxon


BOOTSTRAP_SEED = 20261126
BOOTSTRAP_RESAMPLES = 10000
MIN_TRIALS = 10
BASELINES = ("default-linux", "static-P", "static-E", "oracle-static", "pie-like")


def _is_true(series: pd.Series) -> pd.Series:
    return series.astype(str).str.strip().str.lower().isin(["true", "1"])


def load_and_filter_raw(raw_glob: str = "results/raw/*/runs.csv") -> Tuple[pd.DataFrame, Dict[str, int]]:
    paths = sorted(glob.glob(raw_glob))
    if not paths:
        return pd.DataFrame(), {
            "files": 0, "total_rows": 0, "excluded_warmup": 0,
            "excluded_smoke": 0, "excluded_synthetic": 0, "excluded_invalid": 0,
            "kept_rows": 0,
        }

    df = pd.concat([pd.read_csv(p) for p in paths], ignore_index=True)
    total = len(df)

    mask_warmup = _is_true(df["warmup"]) if "warmup" in df.columns else pd.Series(False, index=df.index)
    mask_smoke = (
        df["session_type"].astype(str).str.strip().str.lower() == "smoke"
        if "session_type" in df.columns else pd.Series(False, index=df.index)
    )
    mask_synth = _is_true(df["synthetic"]) if "synthetic" in df.columns else pd.Series(False, index=df.index)
    mask_invalid = (
        ~_is_true(df["session_valid"])
        if "session_valid" in df.columns else pd.Series(False, index=df.index)
    )
    if "row_valid" in df.columns:
        mask_invalid = mask_invalid | (~_is_true(df["row_valid"]))

    keep = ~(mask_warmup | mask_smoke | mask_synth | mask_invalid)
    counts = {
        "files": len(paths),
        "total_rows": total,
        "excluded_warmup": int(mask_warmup.sum()),
        "excluded_smoke": int(mask_smoke.sum()),
        "excluded_synthetic": int(mask_synth.sum()),
        "excluded_invalid": int(mask_invalid.sum()),
        "kept_rows": int(keep.sum()),
    }
    return df[keep].copy(), counts


def bootstrap_ci(
    vals: List[float],
    n_resamples: int = BOOTSTRAP_RESAMPLES,
    seed: int = BOOTSTRAP_SEED,
) -> Tuple[float, float]:
    if not vals:
        return float("nan"), float("nan")
    rng = random.Random(seed)
    n = len(vals)
    means = sorted(sum(vals[rng.randrange(n)] for _ in range(n)) / n for _ in range(n_resamples))
    return means[int(0.025 * n_resamples)], means[int(0.975 * n_resamples)]


def matched_pairs_rank_biserial(diffs: List[float]) -> float:
    """
    Matched-pairs rank-biserial correlation r = (W+ - W-) / (W+ + W-)
    computed on non-zero paired differences (policy - baseline).
    """
    nz = [d for d in diffs if d != 0.0]
    if not nz:
        return 0.0
    abs_sorted = sorted((abs(d), i, d) for i, d in enumerate(nz))
    ranks = [0.0] * len(nz)
    i = 0
    while i < len(abs_sorted):
        j = i
        while j + 1 < len(abs_sorted) and abs_sorted[j + 1][0] == abs_sorted[i][0]:
            j += 1
        avg_rank = (i + 1 + j + 1) / 2.0
        for k in range(i, j + 1):
            ranks[abs_sorted[k][1]] = avg_rank
        i = j + 1

    w_pos = sum(ranks[idx] for idx, d in enumerate(nz) if d > 0)
    w_neg = sum(ranks[idx] for idx, d in enumerate(nz) if d < 0)
    total = w_pos + w_neg
    return (w_pos - w_neg) / total if total > 0 else 0.0


def paired_wilcoxon_and_effect(
    pol_vals: List[float],
    base_vals: List[float],
    n_resamples: int = BOOTSTRAP_RESAMPLES,
    seed: int = BOOTSTRAP_SEED,
) -> dict:
    """
    Compute paired Wilcoxon signed-rank p-value, rank-biserial r,
    percent change with 95% bootstrap CI, and wins/ties/losses (lower is better).
    """
    diffs = [p - b for p, b in zip(pol_vals, base_vals)]
    wins = sum(1 for d in diffs if d < 0)
    ties = sum(1 for d in diffs if d == 0)
    losses = sum(1 for d in diffs if d > 0)

    nz = [d for d in diffs if d != 0.0]
    if not nz:
        p_val = 1.0
    else:
        _, p_val = wilcoxon(pol_vals, base_vals, zero_method="wilcox")
        p_val = float(p_val)

    r_rb = matched_pairs_rank_biserial(diffs)

    pct_changes = [
        ((p - b) / b * 100.0) if b != 0 else 0.0 for p, b in zip(pol_vals, base_vals)
    ]
    pct_mean = sum(pct_changes) / len(pct_changes)
    pct_lo, pct_hi = bootstrap_ci(pct_changes, n_resamples=n_resamples, seed=seed)

    return {
        "n_pairs": len(diffs),
        "p_value": p_val,
        "rank_biserial_r": r_rb,
        "pct_change_mean": pct_mean,
        "pct_change_ci_lo": pct_lo,
        "pct_change_ci_hi": pct_hi,
        "wins": wins,
        "ties": ties,
        "losses": losses,
    }


def holm_bonferroni(p_values: List[float]) -> List[float]:
    """Return Holm-Bonferroni adjusted p-values preserving original order."""
    m = len(p_values)
    if m == 0:
        return []
    indexed = sorted(enumerate(p_values), key=lambda x: x[1])
    adjusted = [0.0] * m
    running_max = 0.0
    for rank, (orig_idx, p) in enumerate(indexed):
        adj = min(1.0, (m - rank) * p)
        running_max = max(running_max, adj)
        adjusted[orig_idx] = running_max
    return adjusted


def _df_to_latex(df: pd.DataFrame) -> str:
    if df.empty:
        return "% empty table\n"
    cols = list(df.columns)
    align = "l" * len(cols)
    lines = [
        f"\\begin{{tabular}}{{{align}}}",
        "\\hline",
        " & ".join(str(c).replace("_", "\\_") for c in cols) + " \\\\",
        "\\hline",
    ]
    for _, row in df.iterrows():
        cells = [str(row[c]).replace("_", "\\_") for c in cols]
        lines.append(" & ".join(cells) + " \\\\")
    lines.extend(["\\hline", "\\end{tabular}", ""])
    return "\n".join(lines)


def analyze(
    raw_glob: str = "results/raw/*/runs.csv",
    metric: str = "edp_raw",
    out_dir: str = "results",
    n_resamples: int = BOOTSTRAP_RESAMPLES,
    seed: int = BOOTSTRAP_SEED,
) -> Tuple[pd.DataFrame, pd.DataFrame]:
    df, counts = load_and_filter_raw(raw_glob)
    print("=== EXCLUSION REPORT ===")
    for k, v in counts.items():
        print(f"  {k}: {v}")

    if df.empty:
        print("[!] No valid non-warmup, non-smoke, non-synthetic rows found.")
        return pd.DataFrame(), pd.DataFrame()

    summary_rows = []
    for (wl, pol), grp in df.groupby(["workload_variant", "policy"], sort=True):
        vals = pd.to_numeric(grp[metric], errors="coerce").dropna().tolist()
        if len(vals) < MIN_TRIALS:
            summary_rows.append({
                "workload_variant": wl,
                "policy": pol,
                "n": len(vals),
                "status": "INSUFFICIENT",
                "mean": "", "std": "", "median": "", "ci_lo": "", "ci_hi": "",
                "bootstrap_seed": seed, "bootstrap_resamples": n_resamples,
            })
            continue
        arr = np.array(vals, dtype=float)
        lo, hi = bootstrap_ci(vals, n_resamples=n_resamples, seed=seed)
        summary_rows.append({
            "workload_variant": wl,
            "policy": pol,
            "n": len(vals),
            "status": "OK",
            "mean": round(float(np.mean(arr)), 4),
            "std": round(float(np.std(arr, ddof=1)), 4),
            "median": round(float(np.median(arr)), 4),
            "ci_lo": round(lo, 4),
            "ci_hi": round(hi, 4),
            "bootstrap_seed": seed,
            "bootstrap_resamples": n_resamples,
        })

    summary_df = pd.DataFrame(summary_rows)

    pairwise_rows = []
    for wl, wl_df in df.groupby("workload_variant", sort=True):
        policies_present = sorted(wl_df["policy"].unique())
        for base in BASELINES:
            if base not in policies_present:
                continue
            base_sub = wl_df[wl_df["policy"] == base].set_index(["session_id", "trial_index"])
            for pol in policies_present:
                if pol == base:
                    continue
                pol_sub = wl_df[wl_df["policy"] == pol].set_index(["session_id", "trial_index"])
                common = base_sub.index.intersection(pol_sub.index)
                if len(common) < MIN_TRIALS:
                    pairwise_rows.append({
                        "workload_variant": wl, "policy": pol, "baseline": base,
                        "n_pairs": len(common), "status": "INSUFFICIENT",
                        "p_value": float("nan"), "p_holm": float("nan"),
                        "rank_biserial_r": float("nan"),
                        "pct_change_mean": float("nan"),
                        "pct_change_ci_lo": float("nan"), "pct_change_ci_hi": float("nan"),
                        "wins": 0, "ties": 0, "losses": 0,
                    })
                    continue
                p_vals = pd.to_numeric(pol_sub.loc[common, metric], errors="coerce").tolist()
                b_vals = pd.to_numeric(base_sub.loc[common, metric], errors="coerce").tolist()
                res = paired_wilcoxon_and_effect(p_vals, b_vals, n_resamples=n_resamples, seed=seed)
                res.update({"workload_variant": wl, "policy": pol, "baseline": base, "status": "OK"})
                pairwise_rows.append(res)

    pairwise_df = pd.DataFrame(pairwise_rows)
    if not pairwise_df.empty:
        ok_mask = pairwise_df["status"] == "OK"
        ok_p = pairwise_df.loc[ok_mask, "p_value"].tolist()
        print(
            f"\n=== HOLM-BONFERRONI FAMILY ===\n"
            f"  Family: all valid (workload_variant, policy, baseline) paired comparisons "
            f"on '{metric}' (m={len(ok_p)})"
        )
        if ok_p:
            pairwise_df.loc[ok_mask, "p_holm"] = holm_bonferroni(ok_p)

    os.makedirs(out_dir, exist_ok=True)
    summary_df.to_csv(os.path.join(out_dir, "analysis_summary.csv"), index=False)
    pairwise_df.to_csv(os.path.join(out_dir, "analysis_pairwise.csv"), index=False)
    with open(os.path.join(out_dir, "table_summary.tex"), "w") as f:
        f.write(_df_to_latex(summary_df))
    with open(os.path.join(out_dir, "table_pairwise.tex"), "w") as f:
        f.write(_df_to_latex(pairwise_df))

    return summary_df, pairwise_df


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze Pacer raw evaluation sessions.")
    parser.add_argument("--raw-glob", default="results/raw/*/runs.csv")
    parser.add_argument("--metric", default="edp_raw")
    parser.add_argument("--out-dir", default="results")
    args = parser.parse_args()
    summary_df, pairwise_df = analyze(args.raw_glob, metric=args.metric, out_dir=args.out_dir)
    if not summary_df.empty:
        print("\n=== SUMMARY ===")
        print(summary_df.to_string(index=False))
    if not pairwise_df.empty:
        print("\n=== PAIRWISE ===")
        print(pairwise_df.to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
