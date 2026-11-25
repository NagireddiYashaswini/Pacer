#!/usr/bin/env python3
"""
Offline Tuning-Split Profiler for oracle-static and hass-like (Part C).

Runs ONLY on 'tuning' split variants from config/workload_split.yaml,
under host_guard + control-plane pinning (taskset -c 17,35).
Supports all workload wrappers (microbenchmarks, GAPBS, Redis, mixed_tenant).
"""

import argparse
import datetime
import json
import os
import platform
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from energy_meter import RaplEnergyMeter
from host_guard import enforce_host_guard
from policies import PROFILES_DIR, HassLikePolicy
from pool_config import load_pools_config
from workloads import ALL_WORKLOADS
from workloads.base import assert_split_allows, load_workload_split


def profile_wrapper_on_both_pools(wl, v_cfg: dict, pools: dict, meter: RaplEnergyMeter) -> str:
    """Measure wall time and RAPL energy on P-Core and E-Core pools; save oracle profile."""
    results_by_pool = {}
    with tempfile.TemporaryDirectory() as tmpdir:
        for pool_name, pool_cpus in [("P-Core", set(pools["p_cores"])), ("E-Core", set(pools["e_cores"]))]:
            s0 = meter.read_snapshot()
            res = wl.launch(os.path.join(tmpdir, pool_name), variant_cfg=v_cfg, launch_cpus=pool_cpus)
            s1 = meter.read_snapshot()
            if res.exit_status != 0:
                raise RuntimeError(f"Profiling {v_cfg['variant']} failed on {pool_name} (rc={res.exit_status})")
            ed = meter.compute_delta(s0, s1)
            edp = ed.pkg_joules * res.wall_time_sec
            results_by_pool[pool_name] = {
                "wall_time_sec": res.wall_time_sec,
                "energy_joules": ed.pkg_joules,
                "edp": edp,
            }

    best_pool = (
        "P-Core"
        if results_by_pool["P-Core"]["edp"] <= results_by_pool["E-Core"]["edp"]
        else "E-Core"
    )
    os.makedirs(PROFILES_DIR, exist_ok=True)
    ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    app_name = v_cfg["workload"]
    out_path = os.path.join(PROFILES_DIR, f"oracle_{app_name}_t1_{ts}.json")
    suffix = 1
    while os.path.exists(out_path):
        out_path = os.path.join(PROFILES_DIR, f"oracle_{app_name}_t1_{ts}_{suffix}.json")
        suffix += 1

    payload = {
        "policy": "oracle-static",
        "label": "OFFLINE UPPER BOUND for static assignment, not a deployable policy",
        "is_deployable": False,
        "workload": app_name,
        "split": "tuning",
        "kernel_version": platform.release(),
        "rapl_domain": meter.source_name,
        "p_cores": pools["p_cores_str"],
        "e_cores": pools["e_cores_str"],
        "excluded_siblings": pools["excluded_siblings_str"],
        "timestamp_iso": ts,
        "pools": results_by_pool,
        "best_pool_by_edp": best_pool,
    }
    with open(out_path, "w") as f:
        json.dump(payload, f, indent=2)
    return out_path, best_pool


def main() -> int:
    enforce_host_guard()
    pools = load_pools_config()
    os.sched_setaffinity(0, set(pools["control_plane_cpus"]))
    meter = RaplEnergyMeter()

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

        print(f"[PROFILE] {vk} ({wl_name})...")
        prof_path, best_pool = profile_wrapper_on_both_pools(wl, v_cfg, pools, meter)
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
