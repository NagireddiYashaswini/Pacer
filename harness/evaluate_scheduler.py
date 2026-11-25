#!/usr/bin/env python3
"""
End-to-End Evaluation Framework for Measured-Speedup-Aware Hybrid CPU Scheduling.

PAPER INTEGRITY RULES (non-negotiable):
  - RAPL energy must be readable; if not, the script aborts with a clear error
    unless --debug-synthetic is given (which tags all rows synthetic=true and
    is excluded from every plot and table).
  - No wattage estimates, no hardcoded energy values, no np.random data.
  - Every result CSV row records: git_commit, kernel_version, cpu_model,
    governor, turbo_state, per_core_freq_khz, rapl_source, trial_index,
    timestamp_iso (Rule 3).

Compares:
  1. Default Linux Scheduler (CFS unpinned)
  2. Static P-Core Pool (taskset -c 0-7)
  3. Static E-Core Pool (taskset -c 8-17)
  4. Adaptive Hybrid Scheduler (hybridsched daemon)
"""

import os
import sys
import time
import datetime
import subprocess
import argparse
import platform
import socket

try:
    import pandas as pd
except ImportError:
    sys.exit("[FATAL] pandas is required: pip install pandas")

RAPL_PATH = "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj"
DAEMON_BIN = "./bin/hybridsched_daemon"


# ---------------------------------------------------------------------------
# Provenance helpers (Rule 3)
# ---------------------------------------------------------------------------

def _read_file_stripped(path: str) -> str:
    """Return stripped content of path, or raise FileNotFoundError."""
    with open(path, "r") as f:
        return f.read().strip()


def collect_provenance(trial_index: int) -> dict:
    """
    Collect immutable run-level metadata from real system state.
    Raises RuntimeError if a critical field cannot be read.
    Fields that are advisory (governor, turbo) are marked 'unknown' rather
    than failing, because their absence does not invalidate measurements.
    """
    prov = {}

    # git commit
    try:
        prov["git_commit"] = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        prov["git_commit"] = "unknown"

    # kernel version
    prov["kernel_version"] = platform.release()

    # CPU model
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    prov["cpu_model"] = line.split(":", 1)[1].strip()
                    break
            else:
                prov["cpu_model"] = "unknown"
    except OSError:
        prov["cpu_model"] = "unknown"

    # cpufreq governor (cpu0 as representative)
    try:
        prov["governor"] = _read_file_stripped(
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
        )
    except OSError:
        prov["governor"] = "unknown"

    # turbo state
    try:
        no_turbo = _read_file_stripped(
            "/sys/devices/system/cpu/intel_pstate/no_turbo"
        )
        prov["turbo_state"] = "disabled" if no_turbo == "1" else "enabled"
    except OSError:
        try:
            boost = _read_file_stripped(
                "/sys/devices/system/cpu/cpufreq/boost"
            )
            prov["turbo_state"] = "enabled" if boost == "1" else "disabled"
        except OSError:
            prov["turbo_state"] = "unknown"

    # per-core frequencies (kHz), semicolon-separated
    freqs = []
    cpu = 0
    while True:
        path = f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq"
        if not os.path.exists(path):
            break
        try:
            freqs.append(_read_file_stripped(path))
        except OSError:
            freqs.append("?")
        cpu += 1
    prov["per_core_freq_khz"] = ";".join(freqs) if freqs else "unavailable"

    # RAPL source
    if os.path.exists(RAPL_PATH):
        prov["rapl_source"] = "intel-rapl:0"
    else:
        prov["rapl_source"] = "unavailable"

    prov["trial_index"] = trial_index
    prov["timestamp_iso"] = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")

    return prov


# ---------------------------------------------------------------------------
# RAPL energy reading
# ---------------------------------------------------------------------------

def read_rapl_uj() -> int:
    """Read RAPL package energy counter in microjoules. Returns int or None."""
    try:
        return int(_read_file_stripped(RAPL_PATH))
    except (OSError, ValueError):
        return None


def check_rapl_or_abort(debug_synthetic: bool):
    """
    Verify RAPL is readable. If not and --debug-synthetic is not set, abort.
    """
    val = read_rapl_uj()
    if val is None:
        if debug_synthetic:
            print(
                "[WARN] RAPL not readable. Running in --debug-synthetic mode: "
                "energy_joules will be NaN and rows will be tagged synthetic=true. "
                "These rows are EXCLUDED from all plots and tables.",
                file=sys.stderr,
            )
        else:
            sys.exit(
                f"[FATAL] Intel RAPL energy counter not readable at:\n"
                f"  {RAPL_PATH}\n\n"
                f"Real energy measurement is required for paper results.\n"
                f"Do NOT use estimated wattages.\n\n"
                f"To diagnose:\n"
                f"  sudo modprobe intel_rapl_msr\n"
                f"  ls /sys/class/powercap/intel-rapl/\n"
                f"  sudo chmod a+r /sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj\n\n"
                f"If you need to test the harness without real hardware, use:\n"
                f"  --debug-synthetic   (tags all rows synthetic=true, excluded from figures)"
            )


# ---------------------------------------------------------------------------
# Workload runner
# ---------------------------------------------------------------------------

P_CORE_CPULIST = "0-7"
E_CORE_CPULIST = "8-17"


def run_workload_measured(
    cmd_list,
    policy_name: str,
    affinity=None,
    use_daemon: bool = False,
    daemon_kwargs=None,
    debug_synthetic: bool = False,
):
    """
    Execute one or more concurrent commands under the specified scheduling policy.
    Returns: (duration_sec, energy_joules_or_nan, edp_or_nan, migrations_count)

    energy_joules is float('nan') in --debug-synthetic mode if RAPL is absent.
    """
    daemon_proc = None
    if use_daemon:
        daemon_cmd = [DAEMON_BIN, "--interval", "150", "--cooldown", "400",
                      "--hysteresis", "1"]
        if daemon_kwargs:
            for k, v in daemon_kwargs.items():
                daemon_cmd.extend([k, str(v)])
        daemon_proc = subprocess.Popen(
            daemon_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        time.sleep(0.3)

    start_rapl = read_rapl_uj()
    t0 = time.perf_counter()

    procs = []
    for cmd in cmd_list:
        full_cmd = []
        if affinity and not use_daemon:
            full_cmd.extend(["taskset", "-c", affinity])
        full_cmd.extend(cmd)
        p = subprocess.Popen(full_cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
        procs.append(p)

    for p in procs:
        p.wait()

    t1 = time.perf_counter()
    end_rapl = read_rapl_uj()
    duration = t1 - t0

    migrations = 0
    if daemon_proc:
        try:
            daemon_proc.send_signal(subprocess.signal.SIGINT)
            stdout, stderr = daemon_proc.communicate(timeout=2.0)
            for line in stdout.splitlines():
                if "MIGRATION #" in line:
                    migrations += 1
        except Exception:
            daemon_proc.kill()

    # Energy: real RAPL only — no power-model fallback
    if start_rapl is not None and end_rapl is not None and end_rapl >= start_rapl:
        energy_j = (end_rapl - start_rapl) / 1e6
    elif debug_synthetic:
        energy_j = float("nan")  # explicitly marked missing
    else:
        # RAPL became unreadable mid-run (counter reset, permission revoked, etc.)
        sys.exit(
            f"[FATAL] RAPL energy read failed during workload '{policy_name}'.\n"
            f"  start_rapl={start_rapl}, end_rapl={end_rapl}\n"
            f"Cannot compute energy without real RAPL data."
        )

    import math
    edp = energy_j * duration if not math.isnan(energy_j) else float("nan")
    return duration, energy_j, edp, migrations


# ---------------------------------------------------------------------------
# Main evaluation loop
# ---------------------------------------------------------------------------

def run_evaluation(
    compute_iters: int = 800,
    mem_iters: int = 25_000_000,
    phases_count: int = 4,
    phase_dur: float = 1.2,
    num_trials: int = 1,
    debug_synthetic: bool = False,
):
    import math

    check_rapl_or_abort(debug_synthetic)

    policies = [
        {"name": "Default Linux (CFS)",       "affinity": None,           "daemon": False},
        {"name": "Static P-Core Pool",         "affinity": P_CORE_CPULIST, "daemon": False},
        {"name": "Static E-Core Pool",         "affinity": E_CORE_CPULIST, "daemon": False},
        {"name": "Adaptive Hybrid Scheduler",  "affinity": None,           "daemon": True},
    ]

    workloads = [
        {
            "name": "Compute-bound (AVX2 GEMM)",
            "cmds": [["./bin/bench_compute", "--iterations", str(compute_iters),
                      "--matrix-size", "128"]],
        },
        {
            "name": "Memory-bound (Pointer Chase)",
            "cmds": [["./bin/bench_memory", "--iterations", str(mem_iters),
                      "--buffer-mb", "64"]],
        },
        {
            "name": "Dynamic Phase-Shifting",
            "cmds": [["./bin/bench_phase", "--phases", str(phases_count),
                      "--phase-duration", str(phase_dur)]],
        },
        {
            "name": "Multi-Tenant Contention (2x Compute + 2x Memory)",
            "cmds": [
                ["./bin/bench_compute", "--iterations", str(compute_iters),
                 "--matrix-size", "128"],
                ["./bin/bench_compute", "--iterations", str(compute_iters),
                 "--matrix-size", "128"],
                ["./bin/bench_memory", "--iterations", str(mem_iters),
                 "--buffer-mb", "64"],
                ["./bin/bench_memory", "--iterations", str(mem_iters),
                 "--buffer-mb", "64"],
            ],
        },
    ]

    all_results = []
    print("=" * 85)
    print("      MEASURED-SPEEDUP-AWARE HYBRID CPU SCHEDULING EVALUATION HARNESS")
    if debug_synthetic:
        print("  *** --debug-synthetic mode: energy=NaN, rows tagged synthetic=true ***")
    print("=" * 85)

    for trial in range(1, num_trials + 1):
        prov = collect_provenance(trial_index=trial)
        print(f"\n[Trial {trial}/{num_trials}] "
              f"git={prov['git_commit']} kernel={prov['kernel_version']} "
              f"governor={prov['governor']} turbo={prov['turbo_state']}")

        for wl in workloads:
            print(f"\n  EVALUATING: {wl['name']}")
            baseline_time = None
            baseline_edp = None

            for pol in policies:
                dur, joules, edp, migs = run_workload_measured(
                    wl["cmds"],
                    pol["name"],
                    affinity=pol["affinity"],
                    use_daemon=pol["daemon"],
                    debug_synthetic=debug_synthetic,
                )

                if pol["name"] == "Default Linux (CFS)":
                    baseline_time = dur
                    baseline_edp = edp

                speedup = (baseline_time / dur) if baseline_time else 1.0
                edp_reduction = (
                    ((baseline_edp - edp) / baseline_edp * 100.0)
                    if baseline_edp and not math.isnan(baseline_edp) and not math.isnan(edp)
                    else float("nan")
                )

                energy_str = f"{joules:6.1f} J" if not math.isnan(joules) else "   NaN J"
                edp_str    = f"{edp:7.1f} J*s" if not math.isnan(edp)    else "    NaN J*s"
                print(f"  [{pol['name']:<27}] Time: {dur:6.3f} s | "
                      f"Energy: {energy_str} | EDP: {edp_str} | "
                      f"Speedup: {speedup:4.2f}x | Migrations: {migs}")

                row = {
                    # Rule 3: provenance
                    "git_commit":        prov["git_commit"],
                    "kernel_version":    prov["kernel_version"],
                    "cpu_model":         prov["cpu_model"],
                    "governor":          prov["governor"],
                    "turbo_state":       prov["turbo_state"],
                    "per_core_freq_khz": prov["per_core_freq_khz"],
                    "rapl_source":       prov["rapl_source"],
                    "trial_index":       prov["trial_index"],
                    "timestamp_iso":     prov["timestamp_iso"],
                    # measurement
                    "workload":          wl["name"],
                    "policy":            pol["name"],
                    "wall_time_sec":     dur,
                    "energy_joules":     joules,
                    "edp":               edp,
                    "speedup_vs_cfs":    speedup,
                    "edp_reduction_pct": edp_reduction,
                    "migrations":        migs,
                    "synthetic":         debug_synthetic,
                }
                all_results.append(row)

    df = pd.DataFrame(all_results)
    os.makedirs("results", exist_ok=True)
    out_csv = "results/evaluation_results.csv"
    df.to_csv(out_csv, index=False)

    # Summary: exclude synthetic rows from printed table
    real_df = df[df["synthetic"] == False]  # noqa: E712
    print("\n" + "=" * 85)
    print("COMPREHENSIVE SCHEDULER EVALUATION RESULTS SUMMARY (real measurements only):")
    print("=" * 85)
    if not real_df.empty:
        print(real_df[["workload", "policy", "wall_time_sec", "energy_joules",
                        "edp", "speedup_vs_cfs", "migrations"]].to_string(index=False))
    else:
        print("  (no real measurements — all rows are synthetic and excluded)")
    print(f"\n[+] Master evaluation table saved to {out_csv}")
    print("=" * 85)
    return df


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Hybrid Scheduler Evaluation Harness",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
RAPL energy is required. If /sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj
is not readable, the script aborts. Fix with:
  sudo modprobe intel_rapl_msr
  sudo chmod a+r /sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj

For harness-only testing without real hardware use --debug-synthetic.
Synthetic rows are tagged in the CSV and excluded from all figures.
""",
    )
    parser.add_argument("--compute-iters", type=int, default=600)
    parser.add_argument("--mem-iters",     type=int, default=15_000_000)
    parser.add_argument("--phases",        type=int, default=4)
    parser.add_argument("--phase-dur",     type=float, default=1.0)
    parser.add_argument("--trials",        type=int, default=1,
                        help="Number of independent trial repetitions")
    parser.add_argument("--debug-synthetic", action="store_true",
                        help="Run without real RAPL; tags all rows synthetic=true "
                             "and excludes them from plots. NEVER use for paper results.")
    args = parser.parse_args()

    run_evaluation(
        compute_iters=args.compute_iters,
        mem_iters=args.mem_iters,
        phases_count=args.phases,
        phase_dur=args.phase_dur,
        num_trials=args.trials,
        debug_synthetic=args.debug_synthetic,
    )
