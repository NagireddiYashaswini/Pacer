#!/usr/bin/env python3
"""
Validate pie-like raw PMU counters via perf_event_open + <3s sanity check
(bench_compute vs bench_memory on one P-core), saving results/pie_counter_validation.json.
"""

import datetime
import json
import os
import platform
import signal
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from policies import PieLikePolicy
from pool_config import load_pools_config


def _find_worker_tid(pid: int) -> int:
    try:
        tids = sorted(int(x) for x in os.listdir(f"/proc/{pid}/task") if x.isdigit())
        for t in tids:
            if t != pid:
                return t
    except OSError:
        pass
    return pid


def _sample_counter(cmd: list, cpu: int, cfg: int, duration_s: float = 0.25) -> int:
    proc = subprocess.Popen(
        ["taskset", "-c", str(cpu)] + cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        tid = _find_worker_tid(proc.pid)
        fd = PieLikePolicy._perf_event_open_ctypes(PieLikePolicy.PERF_TYPE_RAW, cfg, pid=tid)
        try:
            v0 = struct.unpack("Q", os.read(fd, 8))[0]
            time.sleep(duration_s)
            v1 = struct.unpack("Q", os.read(fd, 8))[0]
            return max(0, v1 - v0)
        finally:
            os.close(fd)
    finally:
        proc.terminate()
        proc.wait(timeout=2.0)


def main() -> int:
    pools = load_pools_config()
    test_cpu = pools["p_cores"][0]

    probe = PieLikePolicy.probe_openable_events()
    failed = {k: v for k, v in probe.items() if not v["openable"]}
    if failed:
        print(f"[FATAL] pie-like counters failed perf_event_open: {failed}", file=sys.stderr)
        return 1

    comp_cmd = ["./bin/bench_compute", "--iterations", "0", "--duration", "2.0", "--matrix-size", "128"]
    mem_cmd = ["./bin/bench_memory", "--iterations", "0", "--duration", "2.0", "--buffer-mb", "64"]

    sanity = {}
    all_ok = True
    for ev_name, cfg in PieLikePolicy.RAW_EVENTS.items():
        vc = _sample_counter(comp_cmd, test_cpu, cfg)
        vm = _sample_counter(mem_cmd, test_cpu, cfg)
        if ev_name == "L1D_PEND_MISS.PENDING":
            direction_ok = vc > 0 and vm > 0
        else:
            # Stall counters must be strictly higher on memory-bound pointer chase
            direction_ok = vc > 0 and vm > vc
        sanity[ev_name] = {
            "encoding": f"0x{cfg:08x}",
            "bench_compute_count": vc,
            "bench_memory_count": vm,
            "direction_ok": direction_ok,
        }
        if not direction_ok:
            all_ok = False

    record = {
        "validated": False,
        "reason": (
            "INVALID per Part B: no Sapphire Rapids perf JSON event table or Intel perfmon "
            "file was found on disk (perf CLI for 6.17.0-1033-oem is missing), so the 4 raw "
            "encodings cannot be verified against an on-disk Sapphire Rapids event table."
        ),
        "timestamp_iso": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "kernel_version": platform.release(),
        "encoding_source": "from memory (UNVERIFIED - no on-disk Sapphire Rapids perf JSON/perfmon table)",
        "test_cpu": test_cpu,
        "openable_probe": probe,
        "sanity_check": sanity,
    }
    os.makedirs("results", exist_ok=True)
    out_path = os.path.join("results", "pie_counter_validation.json")
    with open(out_path, "w") as f:
        json.dump(record, f, indent=2)
    print(json.dumps(record, indent=2))
    return 1


if __name__ == "__main__":
    sys.exit(main())
