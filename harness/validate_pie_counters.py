#!/usr/bin/env python3
"""
Validate pie-like PMU counters against an on-disk Intel perfmon (third_party/perfmon)
or perf JSON event table. Never uses encodings from memory.

Checks:
  1. Loads exact EventCode, UMask, CounterMask from third_party/perfmon (SPR)
     or perf JSON tables, recording file_path, repo_commit, and event entry.
  2. Runs on one P-pool CPU (not CPU 0) and one E-pool CPU.
  3. Normalizes each counter by CPU cycles (frac = count / cycles) and requires
     frac(bench_memory) > frac(bench_compute).
  4. Flags any pair of counters within 5% of each other, or any counter within
     5% of cycles, as suspect.
"""

import datetime
import glob
import json
import os
import platform
import struct
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from policies import PieLikePolicy
from pool_config import load_pools_config

TARGET_EVENTS = (
    "L1D_PEND_MISS.PENDING",
    "CYCLE_ACTIVITY.STALLS_MEM_ANY",
    "CYCLE_ACTIVITY.STALLS_TOTAL",
    "RESOURCE_STALLS.ANY",
)


def find_on_disk_spr_event_table() -> Tuple[Optional[str], Optional[str]]:
    """
    Search third_party/perfmon (preferred) or /usr/share/perf* for Sapphire Rapids
    core event JSON. Returns (file_path, repo_commit_or_none).
    """
    perfmon_root = os.path.join("third_party", "perfmon")
    if os.path.isdir(perfmon_root):
        candidates = sorted(
            glob.glob(os.path.join(perfmon_root, "**", "*sapphirerapids*core*.json"), recursive=True)
            + glob.glob(os.path.join(perfmon_root, "**", "*SPR*core*.json"), recursive=True)
        )
        if candidates:
            commit = "unknown"
            try:
                commit = subprocess.check_output(
                    ["git", "-C", perfmon_root, "rev-parse", "HEAD"],
                    stderr=subprocess.DEVNULL,
                ).decode().strip()
            except Exception:
                pass
            return candidates[0], commit

    for base in ("/usr/share/perf-core", "/usr/share/doc/linux-tools"):
        if os.path.isdir(base):
            cands = sorted(glob.glob(os.path.join(base, "**", "*sapphirerapids*core*.json"), recursive=True))
            if cands:
                return cands[0], "system-package"
    return None, None


def load_events_from_json(json_path: str) -> Dict[str, dict]:
    with open(json_path, "r") as f:
        data = json.load(f)
    entries = data.get("Events", data) if isinstance(data, dict) else data
    found: Dict[str, dict] = {}
    for item in entries:
        if isinstance(item, dict) and item.get("EventName") in TARGET_EVENTS:
            found[item["EventName"]] = item
    return found


def encode_perfmon_entry(entry: dict) -> int:
    ev = int(str(entry["EventCode"]).split(",")[0], 0)
    umask = int(str(entry.get("UMask", "0x0")), 0)
    cmask = int(str(entry.get("CounterMask", "0")), 0)
    return (ev & 0xFF) | ((umask & 0xFF) << 8) | ((cmask & 0xFF) << 24)


def check_direction_and_suspects(
    comp_counts: Dict[str, Tuple[int, int]],
    mem_counts: Dict[str, Tuple[int, int]],
) -> Tuple[dict, List[str], bool]:
    """
    comp_counts / mem_counts map event_name -> (event_count, cycles_count).
    Returns (per_event_report, suspect_flags, all_valid).
    """
    report: Dict[str, dict] = {}
    suspect_flags: List[str] = []
    all_valid = True

    for ev in TARGET_EVENTS:
        vc, cc = comp_counts[ev]
        vm, cm = mem_counts[ev]
        frac_c = (vc / float(cc)) if cc > 0 else 0.0
        frac_m = (vm / float(cm)) if cm > 0 else 0.0
        direction_ok = (vc > 0 and vm > 0 and frac_m > frac_c)

        near_cyc_c = (cc > 0 and abs(vc - cc) / float(cc) <= 0.05)
        near_cyc_m = (cm > 0 and abs(vm - cm) / float(cm) <= 0.05)
        if near_cyc_c:
            suspect_flags.append(f"{ev} on bench_compute ({vc}) equals cycles ({cc}) within 5%")
        if near_cyc_m:
            suspect_flags.append(f"{ev} on bench_memory ({vm}) equals cycles ({cm}) within 5%")

        ev_suspect = near_cyc_c or near_cyc_m
        if not direction_ok or ev_suspect:
            all_valid = False

        report[ev] = {
            "bench_compute_count": vc,
            "bench_compute_cycles": cc,
            "bench_compute_frac": round(frac_c, 6),
            "bench_memory_count": vm,
            "bench_memory_cycles": cm,
            "bench_memory_frac": round(frac_m, 6),
            "direction_ok": direction_ok,
            "suspect": ev_suspect,
        }

    # Check pairwise 5% similarity on bench_memory and bench_compute
    evs = list(TARGET_EVENTS)
    for label, counts_map in (("bench_compute", comp_counts), ("bench_memory", mem_counts)):
        for i in range(len(evs)):
            for j in range(i + 1, len(evs)):
                a = counts_map[evs[i]][0]
                b = counts_map[evs[j]][0]
                mx = max(a, b)
                if mx > 0 and abs(a - b) / float(mx) <= 0.05:
                    suspect_flags.append(
                        f"{label}: {evs[i]} ({a}) and {evs[j]} ({b}) are within 5% of each other"
                    )
                    report[evs[i]]["suspect"] = True
                    report[evs[j]]["suspect"] = True
                    all_valid = False

    return report, suspect_flags, all_valid


def _find_worker_tid(pid: int) -> int:
    try:
        tids = sorted(int(x) for x in os.listdir(f"/proc/{pid}/task") if x.isdigit())
        for t in tids:
            if t != pid:
                return t
    except OSError:
        pass
    return pid


def _sample_with_cycles(cmd: list, cpu: int, raw_cfg: int, duration_s: float = 0.25) -> Tuple[int, int]:
    proc = subprocess.Popen(
        ["taskset", "-c", str(cpu)] + cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        tid = _find_worker_tid(proc.pid)
        fd_cyc = PieLikePolicy._perf_event_open_ctypes(
            PieLikePolicy.PERF_TYPE_HARDWARE, PieLikePolicy.PERF_COUNT_HW_CPU_CYCLES, pid=tid
        )
        fd_ev = PieLikePolicy._perf_event_open_ctypes(PieLikePolicy.PERF_TYPE_RAW, raw_cfg, pid=tid)
        try:
            c0 = struct.unpack("Q", os.read(fd_cyc, 8))[0]
            e0 = struct.unpack("Q", os.read(fd_ev, 8))[0]
            time.sleep(duration_s)
            c1 = struct.unpack("Q", os.read(fd_cyc, 8))[0]
            e1 = struct.unpack("Q", os.read(fd_ev, 8))[0]
            return max(0, e1 - e0), max(0, c1 - c0)
        finally:
            os.close(fd_ev)
            os.close(fd_cyc)
    finally:
        proc.terminate()
        proc.wait(timeout=2.0)


def main() -> int:
    pools = load_pools_config()
    p_non0 = [c for c in pools["p_cores"] if c != 0]
    if not p_non0:
        print("[FATAL] No non-zero P-pool CPU available.", file=sys.stderr)
        return 1
    p_cpu = p_non0[0]
    e_cpu = pools["e_cores"][0]

    json_path, repo_commit = find_on_disk_spr_event_table()
    if not json_path:
        msg = (
            "On-disk Sapphire Rapids event table NOT found at third_party/perfmon "
            "(or /usr/share/perf-core). Encodings must NEVER come from memory. "
            "Clone https://github.com/intel/perfmon into third_party/perfmon and re-run."
        )
        record = {
            "validated": False,
            "reason": msg,
            "timestamp_iso": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "kernel_version": platform.release(),
            "cpus_tested": {"p_cpu": p_cpu, "e_cpu": e_cpu},
        }
        os.makedirs("results", exist_ok=True)
        with open("results/pie_counter_validation.json", "w") as f:
            json.dump(record, f, indent=2)
        print(f"[FATAL] {msg}", file=sys.stderr)
        return 1

    entries = load_events_from_json(json_path)
    missing_entries = [ev for ev in TARGET_EVENTS if ev not in entries]
    if missing_entries:
        print(f"[FATAL] Events missing from {json_path}: {missing_entries}", file=sys.stderr)
        return 1

    comp_cmd = ["./bin/bench_compute", "--iterations", "0", "--duration", "2.0", "--matrix-size", "128"]
    mem_cmd = ["./bin/bench_memory", "--iterations", "0", "--duration", "2.0", "--buffer-mb", "64"]

    cpu_results = {}
    overall_valid = True
    for label, cpu in (("p_pool", p_cpu), ("e_pool", e_cpu)):
        comp_counts = {}
        mem_counts = {}
        for ev in TARGET_EVENTS:
            cfg = encode_perfmon_entry(entries[ev])
            comp_counts[ev] = _sample_with_cycles(comp_cmd, cpu, cfg)
            mem_counts[ev] = _sample_with_cycles(mem_cmd, cpu, cfg)
        rep, suspects, ok = check_direction_and_suspects(comp_counts, mem_counts)
        if not ok:
            overall_valid = False
        cpu_results[label] = {
            "cpu": cpu,
            "events": rep,
            "suspect_flags": suspects,
            "valid": ok,
        }

    record = {
        "validated": overall_valid,
        "timestamp_iso": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "kernel_version": platform.release(),
        "source": {
            "file_path": json_path,
            "repo_commit": repo_commit,
            "event_entries": entries,
        },
        "cpus_tested": {"p_cpu": p_cpu, "e_cpu": e_cpu},
        "results_by_cpu": cpu_results,
    }
    os.makedirs("results", exist_ok=True)
    with open("results/pie_counter_validation.json", "w") as f:
        json.dump(record, f, indent=2)
    print(json.dumps(record, indent=2))
    return 0 if overall_valid else 1


if __name__ == "__main__":
    sys.exit(main())
