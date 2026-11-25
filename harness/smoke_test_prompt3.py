#!/usr/bin/env python3
"""
Short (<5s) smoke test for Prompt 3 (policies, workload split, workload detector).
Executes NO workload data.
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from policies import POLICY_REGISTRY, PieLikePolicy, get_policy
from workloads import detect_all_workloads
from workloads.base import assert_split_allows, load_workload_split


def main() -> int:
    # 1. Policy registry
    expected_policies = {
        "default-linux",
        "static-P",
        "static-E",
        "oracle-static",
        "pie-like",
        "pie-like-topdown",
        "hass-like",
        "pacer-mpki",
        "pacer-speedup",
    }
    assert set(POLICY_REGISTRY.keys()) == expected_policies, (
        f"Policy registry mismatch: {set(POLICY_REGISTRY.keys())}"
    )

    oracle = get_policy("oracle-static")
    assert oracle.is_deployable is False
    assert "OFFLINE UPPER BOUND" in oracle.label

    # 2. pie-like counter probe via perf_event_open
    probe = PieLikePolicy.probe_openable_events()
    print("=== PIE-LIKE PERF_EVENT_OPEN PROBE ===")
    for ev, info in probe.items():
        print(f"  {ev:<32} encoding={info['encoding']} openable={info['openable']} errno={info['errno']}")

    # 3. Verify every policy in POLICY_REGISTRY instantiates and runs setup()/teardown() on a trivial no-op
    print("\n=== POLICY SETUP/TEARDOWN SMOKE TEST (trivial no-op, <5s) ===")
    noop_wl = {"workload": "noop", "split": "tuning"}
    with tempfile.TemporaryDirectory() as tmpdir:
        oracle_prof = os.path.join(tmpdir, "oracle_noop.json")
        with open(oracle_prof, "w") as f:
            json.dump({"best_pool_by_edp": "P-Core", "split": "tuning"}, f)
        hass_sig = os.path.join(tmpdir, "hass_noop.json")
        with open(hass_sig, "w") as f:
            json.dump({"preferred_pool": "P-Core", "split": "tuning"}, f)

        for name in POLICY_REGISTRY:
            kwargs = {}
            if name == "oracle-static":
                kwargs["profile_path"] = oracle_prof
            elif name == "hass-like":
                kwargs["signature_path"] = hass_sig
            pol = get_policy(name, **kwargs)
            pol.setup(noop_wl)
            td = pol.teardown()
            print(f"  + {name:<15} OK (deployable={td['is_deployable']})")

    # 4. Tuning/evaluation split enforcement (Part C)
    variants = load_workload_split()
    assert "gapbs_bfs_tune" in variants and "gapbs_bfs_eval" in variants
    tune_cfg = assert_split_allows("gapbs_bfs_tune", "parameter_selection")
    assert tune_cfg["split"] == "tuning"

    for eval_var in ("bench_compute_eval", "gapbs_bfs_eval", "redis_eval"):
        try:
            assert_split_allows(eval_var, "parameter_selection")
            raise AssertionError(f"Expected refusal for {eval_var}")
        except ValueError:
            pass
    print("\n[OK] Tuning/evaluation split refuses parameter selection on evaluation variants.")

    # 5. Workload detector (no execution)
    available, skipped = detect_all_workloads()
    print("\n=== WORKLOAD DETECTOR REPORT ===")
    print("AVAILABLE:")
    for k, v in available.items():
        print(f"  + {k:<18} : {v}")
    print("SKIPPED:")
    for k, v in skipped.items():
        print(f"  - {k:<18} : {v}")

    print("\n[PASS] Prompt 3 smoke tests passed (<5s, no workload data).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
