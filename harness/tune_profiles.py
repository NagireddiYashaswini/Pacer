#!/usr/bin/env python3
"""
Offline Tuning-Split Profiler for oracle-static and hass-like (Part C).

Runs ONLY on 'tuning' split variants from config/workload_split.yaml,
under host_guard + control-plane pinning (taskset -c 17,35).
Never touches 'evaluation' split variants.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from host_guard import enforce_host_guard
from policies import HassLikePolicy, OracleStaticPolicy
from pool_config import load_pools_config
from workloads import ALL_WORKLOADS
from workloads.base import assert_split_allows, load_workload_split


def main() -> int:
    enforce_host_guard()
    pools = load_pools_config()
    os.sched_setaffinity(0, set(pools["control_plane_cpus"]))

    parser = argparse.ArgumentParser(description="Build tuning-split profiles for oracle-static & hass-like.")
    parser.add_argument("--variants", nargs="*", default=None, help="Specific tuning variants (default: all tuning)")
    args = parser.parse_args()

    all_vars = load_workload_split()
    tune_keys = args.variants or [k for k, v in all_vars.items() if v.get("split") == "tuning"]

    for vk in tune_keys:
        v_cfg = assert_split_allows(vk, "offline_profile")
        wl_name = v_cfg["workload"]
        wl_cls = ALL_WORKLOADS.get(wl_name)
        if not wl_cls:
            continue
        wl = wl_cls()
        ok, reason = wl.check()
        if not ok:
            print(f"[SKIP] {vk}: {reason}")
            continue

        if wl_name in ("bench_compute", "bench_memory", "bench_dram_chase", "bench_phase"):
            cmd = [wl.bin_path] + [str(a) for a in v_cfg.get("args", [])]
        elif wl_name in ("gapbs_bfs", "gapbs_pr"):
            cmd = [
                wl._find_binary(),
                "-g", str(v_cfg.get("graph_scale", 18)),
                "-k", str(v_cfg.get("graph_degree", 16)),
                "-n", str(v_cfg.get("trials", 8)),
            ]
        else:
            print(f"[SKIP] {vk}: offline profile() wrapper only supports single-command binaries (got {wl_name})")
            continue

        print(f"[PROFILE] {vk} ({wl_name})...")
        oracle = OracleStaticPolicy()
        prof_path = oracle.profile(v_cfg, cmd, trial_index=1)
        best_pool = oracle.chosen_pool
        print(f"  -> oracle-static saved {prof_path} (best_pool={best_pool})")

        hass = HassLikePolicy()
        sig_path = hass.build_signature(
            v_cfg,
            ipc_p=0.0, mpki_p=0.0,
            ipc_e=0.0, mpki_e=0.0,
            preferred_pool=best_pool,
        )
        print(f"  -> hass-like saved {sig_path} (preferred_pool={best_pool})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
