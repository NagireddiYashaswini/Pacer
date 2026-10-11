#!/usr/bin/env python3
"""
Comprehensive End-to-End Evaluation Framework for Measured-Speedup-Aware Hybrid CPU Scheduling.
Compares:
  1. Default Linux Scheduler (CFS unpinned)
  2. Static P-Core Pool (taskset -c 0-7)
  3. Static E-Core Pool (taskset -c 8-17)
  4. Adaptive Hybrid Scheduler (hybridsched daemon)

Evaluates:
  - Microbenchmarks: bench_compute, bench_memory, bench_phase
  - Multi-tenant Co-runner: 2x compute + 2x memory concurrently
"""

import os
import sys
import time
import subprocess
import argparse
import pandas as pd
import numpy as np

P_CORE_CPULIST = "0-7"
E_CORE_CPULIST = "8-17"
RAPL_PATH = "/sys/class/powercap/intel-rapl:0/energy_uj"
DAEMON_BIN = "./bin/hybridsched_daemon"

def read_rapl_uj():
    if os.path.exists(RAPL_PATH) and os.access(RAPL_PATH, os.R_OK):
        try:
            with open(RAPL_PATH, "r") as f:
                return int(f.read().strip())
        except Exception:
            return None
    return None

def run_workload_measured(cmd_list, policy_name, affinity=None, use_daemon=False, daemon_kwargs=None):
    """
    Executes one or more concurrent commands under the specified scheduling policy.
    Returns: duration_sec, energy_joules, edp, migrations_count
    """
    daemon_proc = None
    if use_daemon:
        # Start adaptive scheduler daemon
        daemon_cmd = [DAEMON_BIN, "--interval", "150", "--cooldown", "400", "--hysteresis", "1"]
        if daemon_kwargs:
            for k, v in daemon_kwargs.items():
                daemon_cmd.extend([k, str(v)])
        daemon_proc = subprocess.Popen(daemon_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

    start_rapl = read_rapl_uj()
    t0 = time.perf_counter()

    procs = []
    for cmd in cmd_list:
        full_cmd = []
        if affinity and not use_daemon:
            full_cmd.extend(["taskset", "-c", affinity])
        full_cmd.extend(cmd)
        p = subprocess.Popen(full_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        procs.append(p)

    # Wait for all processes to finish
    for p in procs:
        p.wait()

    t1 = time.perf_counter()
    end_rapl = read_rapl_uj()
    duration = t1 - t0

    migrations = 0
    if daemon_proc:
        # Terminate daemon cleanly
        try:
            daemon_proc.send_signal(subprocess.signal.SIGINT)
            stdout, stderr = daemon_proc.communicate(timeout=2.0)
            # Count migrations from daemon output
            for line in stdout.splitlines():
                if "MIGRATION #" in line:
                    migrations += 1
        except Exception:
            daemon_proc.kill()

    # Calculate Joules (strictly from hardware RAPL counters)
    if start_rapl is not None and end_rapl is not None and end_rapl >= start_rapl:
        energy_j = (end_rapl - start_rapl) / 1e6
        edp = energy_j * duration
    else:
        # Hardware RAPL counters not accessible (requires root / CAP_SYS_RAWIO)
        # Never fabricate power numbers with hardcoded constants
        energy_j = None
        edp = None

    return duration, energy_j, edp, migrations

def run_evaluation(compute_iters=800, mem_iters=25000000, phases_count=4, phase_dur=1.2):
    policies = [
        {"name": "Default Linux (CFS)",       "affinity": None,           "daemon": False},
        {"name": "Static P-Core Pool",        "affinity": P_CORE_CPULIST, "daemon": False},
        {"name": "Static E-Core Pool",        "affinity": E_CORE_CPULIST, "daemon": False},
        {"name": "Adaptive Hybrid Scheduler", "affinity": None,           "daemon": True},
    ]

    workloads = [
        {
            "name": "Compute-bound (AVX2 GEMM)",
            "cmds": [["./bin/bench_compute", "--iterations", str(compute_iters), "--matrix-size", "128"]]
        },
        {
            "name": "Memory-bound (Pointer Chase)",
            "cmds": [["./bin/bench_memory", "--iterations", str(mem_iters), "--buffer-mb", "64"]]
        },
        {
            "name": "Dynamic Phase-Shifting",
            "cmds": [["./bin/bench_phase", "--phases", str(phases_count), "--phase-duration", str(phase_dur)]]
        },
        {
            "name": "Multi-Tenant Contention (2x Compute + 2x Memory)",
            "cmds": [
                ["./bin/bench_compute", "--iterations", str(compute_iters), "--matrix-size", "128"],
                ["./bin/bench_compute", "--iterations", str(compute_iters), "--matrix-size", "128"],
                ["./bin/bench_memory", "--iterations", str(mem_iters), "--buffer-mb", "64"],
                ["./bin/bench_memory", "--iterations", str(mem_iters), "--buffer-mb", "64"]
            ]
        }
    ]

    all_results = []
    print("=" * 85)
    print("      MEASURED-SPEEDUP-AWARE HYBRID CPU SCHEDULING EVALUATION HARNESS")
    print(f"  P-Cores: {P_CORE_CPULIST} | E-Cores: {E_CORE_CPULIST} | Host: Intel Xeon w5-2565X")
    print("=" * 85)

    for wl in workloads:
        print(f"\n=====================================================================")
        print(f"  EVALUATING: {wl['name']}")
        print(f"=====================================================================")

        baseline_time = None
        baseline_edp = None

        for pol in policies:
            dur, joules, edp, migs = run_workload_measured(
                wl["cmds"], pol["name"], affinity=pol["affinity"], use_daemon=pol["daemon"]
            )

            if pol["name"] == "Default Linux (CFS)":
                baseline_time = dur
                baseline_edp = edp

            speedup_vs_cfs = (baseline_time / dur) if baseline_time else 1.0
            if baseline_edp is not None and edp is not None:
                edp_reduction_pct = ((baseline_edp - edp) / baseline_edp * 100.0)
                edp_str = f"EDP: {edp:7.1f} J*s"
                energy_str = f"Energy: {joules:6.1f} J"
            else:
                edp_reduction_pct = None
                edp_str = "EDP: N/A (unprivileged)"
                energy_str = "Energy: N/A (unprivileged)"

            print(f"  [{pol['name']:<27}] Time: {dur:6.3f} s | {energy_str} | "
                  f"{edp_str} | Speedup: {speedup_vs_cfs:4.2f}x | Migrations: {migs}")

            all_results.append({
                "workload": wl["name"],
                "policy": pol["name"],
                "wall_time_sec": dur,
                "energy_joules": joules if joules is not None else np.nan,
                "edp": edp if edp is not None else np.nan,
                "speedup_vs_cfs": speedup_vs_cfs,
                "edp_reduction_pct": edp_reduction_pct if edp_reduction_pct is not None else np.nan,
                "migrations": migs
            })

    df = pd.DataFrame(all_results)
    os.makedirs("results", exist_ok=True)
    out_csv = "results/evaluation_results.csv"
    df.to_csv(out_csv, index=False)

    print("\n" + "=" * 85)
    print("COMPREHENSIVE SCHEDULER EVALUATION RESULTS SUMMARY:")
    print("=" * 85)
    print(df[["workload", "policy", "wall_time_sec", "energy_joules", "edp", "speedup_vs_cfs", "migrations"]].to_string(index=False))
    print(f"\n[+] Master evaluation table saved to {out_csv}")
    print("=" * 85)
    return df

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Hybrid Scheduler Evaluation Harness")
    parser.add_argument("--compute-iters", type=int, default=600, help="Compute benchmark iterations")
    parser.add_argument("--mem-iters", type=int, default=15000000, help="Memory benchmark iterations")
    parser.add_argument("--phases", type=int, default=4, help="Phase shifter phase count")
    parser.add_argument("--phase-dur", type=float, default=1.0, help="Phase shifter duration per phase")
    args = parser.parse_args()

    run_evaluation(args.compute_iters, args.mem_iters, args.phases, args.phase_dur)
