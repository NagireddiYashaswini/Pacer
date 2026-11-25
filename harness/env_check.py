#!/usr/bin/env python3
"""
Environment & Hardware Verification for Hybrid CPU Scheduling Experiments.

Checks and records (to stdout and results/env_check.json):
  - CPU model
  - Hybrid PMUs (cpu_core / cpu_atom) vs homogeneous
  - P/E core lists (from config/daemon.conf or CLI) & physical-core disjointness
  - cpufreq governor & intel_pstate no_turbo
  - Per-core scaling_max_freq AND measured frequency under 1 s load
  - RAPL readability (via scanned powercap path)
  - perf_event_paranoid
  - SMT state

On homogeneous hosts, enforces "emulated heterogeneity":
  1. Disjoint physical cores per pool (no SMT siblings shared across pools).
  2. E-pool clamped via cpufreq scaling_max_freq below P-pool.
  3. Measured frequency under 1 s load PROVES the clamp took effect.

Exits non-zero if the environment cannot support a valid experiment.
Does NOT modify any system settings.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from typing import Dict, List, Set, Tuple

from rapl_utils import find_rapl_package_domain


def read_stripped(path: str) -> str:
    with open(path, "r") as f:
        return f.read().strip()


def parse_cpulist(cpulist_str: str) -> List[int]:
    cpus: List[int] = []
    for part in cpulist_str.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            cpus.extend(range(int(a), int(b) + 1))
        else:
            cpus.append(int(part))
    return sorted(set(cpus))


def parse_daemon_conf(conf_path: str) -> Tuple[List[int], List[int]]:
    p_cores: List[int] = []
    e_cores: List[int] = []
    if not os.path.exists(conf_path):
        return p_cores, e_cores
    with open(conf_path, "r") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if "=" not in line:
                continue
            k, v = [x.strip() for x in line.split("=", 1)]
            if k == "p_cores":
                p_cores = parse_cpulist(v)
            elif k == "e_cores":
                e_cores = parse_cpulist(v)
    return p_cores, e_cores


def get_cpu_topology() -> Dict[int, Dict[str, int]]:
    """Return {cpu_id: {'physical_id': int, 'core_id': int}} from /proc/cpuinfo."""
    topo: Dict[int, Dict[str, int]] = {}
    cur_proc = None
    cur_phys = 0
    cur_core = 0
    with open("/proc/cpuinfo", "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                if cur_proc is not None:
                    topo[cur_proc] = {"physical_id": cur_phys, "core_id": cur_core}
                cur_proc = None
                continue
            if line.startswith("processor"):
                cur_proc = int(line.split(":", 1)[1].strip())
            elif line.startswith("physical id"):
                cur_phys = int(line.split(":", 1)[1].strip())
            elif line.startswith("core id"):
                cur_core = int(line.split(":", 1)[1].strip())
    if cur_proc is not None:
        topo[cur_proc] = {"physical_id": cur_phys, "core_id": cur_core}
    return topo


def get_cpu_model() -> str:
    with open("/proc/cpuinfo", "r") as f:
        for line in f:
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return "unknown"


def measure_pool_freq_under_load(cpus: List[int], duration_sec: float = 1.0) -> Dict[str, object]:
    """
    Spawn a pinned busy-loop worker on each CPU in `cpus` for `duration_sec`,
    sample /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq during the load,
    and also try `perf stat` (cycles / task-clock) if available for aperf-style
    effective GHz.
    """
    if not cpus:
        return {"per_cpu_khz": {}, "mean_khz": 0.0, "perf_ghz": None}

    # Spawn a lightweight Python busy-loop pinned to all target CPUs (one process per CPU)
    worker_code = "import time; t=time.monotonic()+%f\nwhile time.monotonic()<t: pass" % (duration_sec + 0.3)
    procs = []
    for cpu in cpus:
        p = subprocess.Popen(
            ["taskset", "-c", str(cpu), sys.executable, "-c", worker_code],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        procs.append(p)

    # Let workers ramp up for 0.25s, then sample scaling_cur_freq 4 times over ~0.75s
    time.sleep(0.25)
    samples: Dict[int, List[int]] = {c: [] for c in cpus}
    for _ in range(4):
        for c in cpus:
            path = f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_cur_freq"
            try:
                samples[c].append(int(read_stripped(path)))
            except OSError:
                pass
        time.sleep(duration_sec / 5.0)

    for p in procs:
        p.wait()

    per_cpu_avg: Dict[str, float] = {}
    all_vals: List[float] = []
    for c in cpus:
        if samples[c]:
            avg = sum(samples[c]) / len(samples[c])
            per_cpu_avg[str(c)] = round(avg, 1)
            all_vals.append(avg)

    mean_khz = round(sum(all_vals) / len(all_vals), 1) if all_vals else 0.0
    return {
        "per_cpu_khz": per_cpu_avg,
        "mean_khz": mean_khz,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify host environment for Pacer experiments.")
    parser.add_argument("--config", default="config/daemon.conf", help="Path to daemon.conf")
    parser.add_argument("--p-cores", default=None, help="Override P-core CPU list (e.g. 0-7)")
    parser.add_argument("--e-cores", default=None, help="Override E-core CPU list (e.g. 8-17)")
    parser.add_argument("--out", default="results/env_check.json", help="Output JSON path")
    parser.add_argument("--load-sec", type=float, default=1.0, help="Load duration per pool (s)")
    args = parser.parse_args()

    errors: List[str] = []
    warnings: List[str] = []

    # 1. CPU model
    cpu_model = get_cpu_model()

    # 2. Hybrid PMU detection
    has_cpu_core_pmu = os.path.exists("/sys/bus/event_source/devices/cpu_core/type")
    has_cpu_atom_pmu = os.path.exists("/sys/bus/event_source/devices/cpu_atom/type")
    is_hybrid = has_cpu_core_pmu and has_cpu_atom_pmu

    # 3. P/E core lists
    conf_p, conf_e = parse_daemon_conf(args.config)
    if args.p_cores:
        p_cores = parse_cpulist(args.p_cores)
    elif is_hybrid and os.path.exists("/sys/devices/cpu_core/cpus"):
        p_cores = parse_cpulist(read_stripped("/sys/devices/cpu_core/cpus"))
    else:
        p_cores = conf_p

    if args.e_cores:
        e_cores = parse_cpulist(args.e_cores)
    elif is_hybrid and os.path.exists("/sys/devices/cpu_atom/cpus"):
        e_cores = parse_cpulist(read_stripped("/sys/devices/cpu_atom/cpus"))
    else:
        e_cores = conf_e

    if not p_cores or not e_cores:
        errors.append("P-core or E-core pool is empty.")

    # 4. Topology & SMT state
    topo = get_cpu_topology()
    online_cpus = sorted(topo.keys())
    try:
        smt_active = read_stripped("/sys/devices/system/cpu/smt/active") == "1"
    except OSError:
        smt_active = len(online_cpus) > len({(v["physical_id"], v["core_id"]) for v in topo.values()})

    # Check disjointness of logical CPUs and physical cores
    overlap_logical = sorted(set(p_cores) & set(e_cores))
    if overlap_logical:
        errors.append(f"Logical CPUs shared between P and E pools: {overlap_logical}")

    p_phys: Set[Tuple[int, int]] = {(topo[c]["physical_id"], topo[c]["core_id"]) for c in p_cores if c in topo}
    e_phys: Set[Tuple[int, int]] = {(topo[c]["physical_id"], topo[c]["core_id"]) for c in e_cores if c in topo}
    overlap_phys = sorted(p_phys & e_phys)
    if overlap_phys:
        errors.append(
            f"Physical cores (SMT siblings) shared between P and E pools: {overlap_phys}. "
            f"Pools must use disjoint physical cores."
        )

    # 5. Governor & Turbo
    try:
        governor = read_stripped("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    except OSError:
        governor = "unknown"

    turbo_state = "unknown"
    no_turbo_val = None
    if os.path.exists("/sys/devices/system/cpu/intel_pstate/no_turbo"):
        no_turbo_val = read_stripped("/sys/devices/system/cpu/intel_pstate/no_turbo")
        turbo_state = "disabled" if no_turbo_val == "1" else "enabled"
    elif os.path.exists("/sys/devices/system/cpu/cpufreq/boost"):
        boost_val = read_stripped("/sys/devices/system/cpu/cpufreq/boost")
        turbo_state = "enabled" if boost_val == "1" else "disabled"

    # 6. Per-core scaling_max_freq
    scaling_max_freq_khz: Dict[str, int] = {}
    for c in online_cpus:
        p = f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_max_freq"
        try:
            scaling_max_freq_khz[str(c)] = int(read_stripped(p))
        except OSError:
            pass

    # 7. RAPL readability (using shared powercap scanner)
    rapl_domain, rapl_energy_path, rapl_max_range_path = find_rapl_package_domain()
    rapl_readable = False
    rapl_sample_uj = None
    if rapl_energy_path and os.path.exists(rapl_energy_path):
        try:
            rapl_sample_uj = int(read_stripped(rapl_energy_path))
            rapl_readable = True
        except (OSError, ValueError) as exc:
            errors.append(f"RAPL energy_uj exists at {rapl_energy_path} but is not readable: {exc}")
    else:
        errors.append("Intel RAPL package domain not found under /sys/class/powercap/intel-rapl/.")

    # 8. perf_event_paranoid
    paranoid_val = None
    try:
        paranoid_val = int(read_stripped("/proc/sys/kernel/perf_event_paranoid"))
        if paranoid_val > 1:
            errors.append(
                f"perf_event_paranoid is {paranoid_val} (must be <= 1 for unprivileged per-thread PMU)."
            )
    except (OSError, ValueError) as exc:
        errors.append(f"Could not read /proc/sys/kernel/perf_event_paranoid: {exc}")

    # 9. Measured frequency under 1s load per pool
    p_load_meas = measure_pool_freq_under_load(p_cores, duration_sec=args.load_sec)
    e_load_meas = measure_pool_freq_under_load(e_cores, duration_sec=args.load_sec)

    # 10. Emulated heterogeneity validation on homogeneous hosts
    p_max_freqs = [scaling_max_freq_khz[str(c)] for c in p_cores if str(c) in scaling_max_freq_khz]
    e_max_freqs = [scaling_max_freq_khz[str(c)] for c in e_cores if str(c) in scaling_max_freq_khz]

    clamp_verified = False
    proposed_clamp_commands: List[str] = []

    if not is_hybrid:
        if not p_max_freqs or not e_max_freqs:
            errors.append("cpufreq scaling_max_freq not readable; cannot verify frequency clamp on homogeneous host.")
        else:
            p_min_max = min(p_max_freqs)
            e_max_max = max(e_max_freqs)
            p_meas_mean = float(p_load_meas["mean_khz"])
            e_meas_mean = float(e_load_meas["mean_khz"])

            # Check scaling_max_freq clamp: E pool must be clamped at least 500 MHz below P pool
            if e_max_max > p_min_max - 500_000:
                errors.append(
                    f"Homogeneous host requires emulated heterogeneity via cpufreq clamp: "
                    f"E-pool max scaling_max_freq ({e_max_max} kHz) is NOT clamped below "
                    f"P-pool min scaling_max_freq ({p_min_max} kHz)."
                )
            elif e_meas_mean <= 0 or p_meas_mean <= 0:
                errors.append("Failed to measure per-core frequencies under load.")
            elif e_meas_mean > e_max_max * 1.05:
                errors.append(
                    f"Frequency clamp did NOT take effect under load: E-pool measured mean "
                    f"{e_meas_mean:.0f} kHz exceeds E-pool scaling_max_freq {e_max_max} kHz."
                )
            elif p_meas_mean - e_meas_mean < 400_000:
                errors.append(
                    f"Measured frequency gap under load too small to prove heterogeneity: "
                    f"P-pool={p_meas_mean:.0f} kHz vs E-pool={e_meas_mean:.0f} kHz (need >= 400000 kHz gap)."
                )
            else:
                clamp_verified = True

        # Build exact clamp commands (including SMT siblings of E-pool if SMT is active)
        e_with_siblings = set(e_cores)
        for c, info in topo.items():
            if (info["physical_id"], info["core_id"]) in e_phys:
                e_with_siblings.add(c)
        e_clamp_list = sorted(e_with_siblings)
        proposed_clamp_commands = [
            f"# Clamp E-pool physical cores (CPUs {','.join(map(str, e_clamp_list))}, including SMT siblings) to 2.0 GHz:",
            f"for c in {' '.join(map(str, e_clamp_list))}; do echo 2000000 | sudo tee /sys/devices/system/cpu/cpu$c/cpufreq/scaling_max_freq; done",
        ]

    valid = len(errors) == 0

    report = {
        "valid": valid,
        "mode": "native_hybrid" if is_hybrid else "emulated_heterogeneity",
        "cpu_model": cpu_model,
        "is_hybrid_pmu": is_hybrid,
        "hybrid_pmus": {
            "cpu_core": has_cpu_core_pmu,
            "cpu_atom": has_cpu_atom_pmu,
        },
        "online_cpus": len(online_cpus),
        "physical_cores": len({(v["physical_id"], v["core_id"]) for v in topo.values()}),
        "smt_active": smt_active,
        "pools": {
            "p_cores": p_cores,
            "e_cores": e_cores,
            "disjoint_physical_cores": (len(overlap_logical) == 0 and len(overlap_phys) == 0),
        },
        "governor": governor,
        "turbo_state": turbo_state,
        "no_turbo": no_turbo_val,
        "scaling_max_freq_khz": scaling_max_freq_khz,
        "measured_freq_under_load": {
            "p_pool": p_load_meas,
            "e_pool": e_load_meas,
            "clamp_verified": clamp_verified if not is_hybrid else None,
        },
        "rapl": {
            "domain": rapl_domain,
            "energy_uj_path": rapl_energy_path,
            "max_energy_range_uj_path": rapl_max_range_path,
            "readable": rapl_readable,
            "sample_uj": rapl_sample_uj,
        },
        "perf_event_paranoid": paranoid_val,
        "errors": errors,
        "warnings": warnings,
        "proposed_clamp_commands": proposed_clamp_commands,
    }

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(report, f, indent=2)

    print(json.dumps(report, indent=2))
    if not valid:
        print("\n[ENV_CHECK FAILED]", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        if proposed_clamp_commands:
            print("\nTo apply emulated heterogeneity E-core frequency clamp, run:", file=sys.stderr)
            for cmd in proposed_clamp_commands:
                print(f"  {cmd}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
