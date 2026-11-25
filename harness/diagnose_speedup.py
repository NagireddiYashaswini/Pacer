#!/usr/bin/env python3
"""
Summarize 30-trial randomized P/E diagnostic measurements from
results/diagnose_speedup_samples.csv with 95% bootstrap confidence intervals.
"""

import math
import random
import pandas as pd


def bootstrap_mean_ci(vals, n_boot=2000, seed=42):
    rng = random.Random(seed)
    n = len(vals)
    means = []
    for _ in range(n_boot):
        s = sum(vals[rng.randrange(n)] for _ in range(n)) / n
        means.append(s)
    means.sort()
    return means[int(0.025 * n_boot)], means[int(0.975 * n_boot)]


def bootstrap_ratio_ci(p_vals, e_vals, n_boot=2000, seed=42):
    rng = random.Random(seed)
    n = len(p_vals)
    ratios = []
    for _ in range(n_boot):
        mp = sum(p_vals[rng.randrange(n)] for _ in range(n)) / n
        me = sum(e_vals[rng.randrange(n)] for _ in range(n)) / n
        ratios.append(mp / me if me > 0 else 0.0)
    ratios.sort()
    return ratios[int(0.025 * n_boot)], ratios[int(0.975 * n_boot)]


def main():
    df = pd.read_csv("results/diagnose_speedup_samples.csv")

    print("\n=== CPU PINNING & NUMA VERIFICATION ===")
    mismatch = df[df["pinned_cpu"] != df["actual_cpu"]]
    print(f"Total samples: {len(df)} | CPU pin mismatches (pinned != actual): {len(mismatch)}")
    for b in df["benchmark"].unique():
        for p in ["P-Core", "E-Core"]:
            sub = df[(df["benchmark"] == b) & (df["pool"] == p)]
            cpus = sorted(sub["actual_cpu"].unique().tolist())
            print(f"  {b:<18} {p:<7} actual_cpus={cpus} (n={len(sub)})")

    print("\n=== SUMMARY TABLE (30 randomized trials per benchmark) ===")
    header = (
        f"{'Benchmark':<18} {'Pool':<7} {'n':>3} "
        f"{'instr/s mean ± std (95% CI)':<36} "
        f"{'Cycles':>12} {'Instr':>12} {'IPC':>6} {'LLC Miss':>10} {'Freq(kHz)':>10} "
        f"{'Speedup (95% CI)':<22} {'FreqRatio':>9}"
    )
    print(header)
    print("-" * len(header))

    for b in df["benchmark"].unique():
        sub_p = df[(df["benchmark"] == b) & (df["pool"] == "P-Core")].sort_values("trial_index")
        sub_e = df[(df["benchmark"] == b) & (df["pool"] == "E-Core")].sort_values("trial_index")

        p_ips = sub_p["instr_per_sec"].tolist()
        e_ips = sub_e["instr_per_sec"].tolist()
        s_ratio = (sum(p_ips) / len(p_ips)) / (sum(e_ips) / len(e_ips))
        s_lo, s_hi = bootstrap_ratio_ci(p_ips, e_ips)
        f_ratio = sub_p["freq_khz"].mean() / sub_e["freq_khz"].mean()

        for pool_name, sub, s_str, fr_str in [
            ("P-Core", sub_p, f"{s_ratio:.4f}x [{s_lo:.4f}, {s_hi:.4f}]", f"{f_ratio:.4f}x"),
            ("E-Core", sub_e, "", ""),
        ]:
            ips_vals = sub["instr_per_sec"].tolist()
            mean_v = sum(ips_vals) / len(ips_vals)
            var_v = sum((x - mean_v) ** 2 for x in ips_vals) / (len(ips_vals) - 1)
            std_v = math.sqrt(var_v)
            ci_lo, ci_hi = bootstrap_mean_ci(ips_vals)
            ips_str = f"{mean_v/1e6:7.2f}M ± {std_v/1e6:5.2f}M [{ci_lo/1e6:7.2f}M,{ci_hi/1e6:7.2f}M]"
            print(
                f"{b:<18} {pool_name:<7} {len(sub):>3} "
                f"{ips_str:<36} "
                f"{sub['cycles'].mean():>12.0f} {sub['instructions'].mean():>12.0f} "
                f"{sub['ipc'].mean():>6.3f} {sub['llc_misses'].mean():>10.0f} "
                f"{sub['freq_khz'].mean():>10.0f} "
                f"{s_str:<22} {fr_str:>9}"
            )

    print("\n=== UNC0RE FREQUENCY (sysfs) ===")
    print(f"min_freq_khz={df['uncore_min_khz'].iloc[0]:.0f}, max_freq_khz={df['uncore_max_khz'].iloc[0]:.0f}")


if __name__ == "__main__":
    main()
