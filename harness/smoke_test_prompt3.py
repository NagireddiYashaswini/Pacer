#!/usr/bin/env python3
"""
Short (<5s) smoke test for Prompt 3 (policies, workload split, workload detector).
Executes NO workload data.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from policies import POLICY_REGISTRY, get_policy
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

    # 2. pie-like must fail loudly when stall/MLP counters are absent
    pie = get_policy("pie-like")
    exposed = pie.detect_available_pmu_events()
    if any(ev not in exposed for ev in pie.REQUIRED_EVENTS):
        try:
            pie.setup()
            raise AssertionError("pie-like should have failed loudly on missing counters")
        except RuntimeError as exc:
            print("[OK] pie-like failed loudly as required:")
            for line in str(exc).splitlines():
                print(f"     {line}")

    # 3. Tuning/evaluation split enforcement (Part C)
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
    print("[OK] Tuning/evaluation split refuses parameter selection on evaluation variants.")

    # 4. Workload detector (no execution)
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
