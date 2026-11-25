#!/usr/bin/env python3
"""
Unit tests for RAPL wraparound math, refuse-to-run guards, and analyze.py
exclusion logic. Uses TemporaryDirectory only; never writes to results/.
"""

import os
import sys
import tempfile
import unittest

import pandas as pd

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "harness"))

from analyze import analyze, load_and_filter_raw
from energy_meter import compute_rapl_delta_uj
from host_guard import check_host_guard
from run_experiments import check_pie_like_validated, create_session_dir
from validate_pie_counters import check_direction_and_suspects
from workloads import ALL_WORKLOADS
from workloads.base import assert_split_allows


class TestHarnessGuards(unittest.TestCase):
    def test_rapl_wraparound(self):
        max_r = 262143328850
        self.assertEqual(compute_rapl_delta_uj(1000, 2500, max_r), 1500)
        self.assertEqual(compute_rapl_delta_uj(max_r - 100, 50, max_r), 150)
        with self.assertRaises(ValueError):
            compute_rapl_delta_uj(1000, 500, 0)

    def test_refuse_evaluation_split_for_tuning(self):
        with self.assertRaises(ValueError):
            assert_split_allows("bench_compute_eval", "tuning")
        cfg = assert_split_allows("bench_compute_tune", "tuning")
        self.assertEqual(cfg["split"], "tuning")

    def test_refuse_existing_session_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            old_cwd = os.getcwd()
            os.chdir(tmp)
            try:
                d = create_session_dir("s1", "evaluation", smoke=False)
                self.assertTrue(os.path.isdir(d))
                with self.assertRaises(FileExistsError):
                    create_session_dir("s1", "evaluation", smoke=False)
            finally:
                os.chdir(old_cwd)

    def test_analyze_exclusions_and_insufficient(self):
        with tempfile.TemporaryDirectory() as tmp:
            sess_dir = os.path.join(tmp, "raw", "sess1")
            out_dir = os.path.join(tmp, "out")
            os.makedirs(sess_dir, exist_ok=True)
            rows = [
                # 1 warmup (excluded)
                {"session_id": "sess1", "trial_index": 0, "workload_variant": "w1", "policy": "default-linux",
                 "edp_raw": 10.0, "warmup": "true", "session_type": "evaluation", "synthetic": "false", "session_valid": "true"},
                # 1 smoke (excluded)
                {"session_id": "sess1", "trial_index": 1, "workload_variant": "w1", "policy": "default-linux",
                 "edp_raw": 10.0, "warmup": "false", "session_type": "smoke", "synthetic": "false", "session_valid": "true"},
                # 1 synthetic (excluded)
                {"session_id": "sess1", "trial_index": 2, "workload_variant": "w1", "policy": "default-linux",
                 "edp_raw": 10.0, "warmup": "false", "session_type": "evaluation", "synthetic": "true", "session_valid": "true"},
                # 1 invalid session (excluded)
                {"session_id": "sess1", "trial_index": 3, "workload_variant": "w1", "policy": "default-linux",
                 "edp_raw": 10.0, "warmup": "false", "session_type": "evaluation", "synthetic": "false", "session_valid": "false"},
                # 3 valid rows (< 10 => INSUFFICIENT)
                {"session_id": "sess1", "trial_index": 4, "workload_variant": "w1", "policy": "default-linux",
                 "edp_raw": 10.0, "warmup": "false", "session_type": "evaluation", "synthetic": "false", "session_valid": "true"},
                {"session_id": "sess1", "trial_index": 5, "workload_variant": "w1", "policy": "default-linux",
                 "edp_raw": 11.0, "warmup": "false", "session_type": "evaluation", "synthetic": "false", "session_valid": "true"},
                {"session_id": "sess1", "trial_index": 6, "workload_variant": "w1", "policy": "default-linux",
                 "edp_raw": 12.0, "warmup": "false", "session_type": "evaluation", "synthetic": "false", "session_valid": "true"},
            ]
            csv_p = os.path.join(sess_dir, "runs.csv")
            pd.DataFrame(rows).to_csv(csv_p, index=False)

            kept, counts = load_and_filter_raw(os.path.join(tmp, "raw", "*", "runs.csv"))
            self.assertEqual(counts["excluded_warmup"], 1)
            self.assertEqual(counts["excluded_smoke"], 1)
            self.assertEqual(counts["excluded_synthetic"], 1)
            self.assertEqual(counts["excluded_invalid"], 1)
            self.assertEqual(len(kept), 3)

            summary_df, _ = analyze(os.path.join(tmp, "raw", "*", "runs.csv"), out_dir=out_dir, n_resamples=100)
            self.assertEqual(summary_df.iloc[0]["status"], "INSUFFICIENT")

    def test_pie_gate_refuses_without_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            ok, msg = check_pie_like_validated(os.path.join(tmp, "nonexistent.json"))
            self.assertFalse(ok)
            self.assertIn("excluded", msg)

    def test_host_guard_checks(self):
        ok = check_host_guard(
            pid1_comm="systemd",
            pid1_cgroup="0::/init.scope",
            cpus_allowed_str="17,35",
            control_plane_cpus={17, 35},
        )
        self.assertEqual(ok, [])

        fails = check_host_guard(
            pid1_comm="exe",
            pid1_cgroup="0::/user.slice/app.scope",
            cpus_allowed_str="0-35",
            control_plane_cpus={17, 35},
        )
        self.assertEqual(len(fails), 3)
        self.assertIn("(a)", fails[0])
        self.assertIn("(b)", fails[1])
        self.assertIn("(c)", fails[2])

    def test_pie_direction_and_suspect_flags(self):
        # Reproduce L1D_PEND_MISS.PENDING compute > memory + 3 counters near cycles
        comp = {
            "L1D_PEND_MISS.PENDING": (2592148527, 775000000),
            "CYCLE_ACTIVITY.STALLS_MEM_ANY": (49028240, 775000000),
            "CYCLE_ACTIVITY.STALLS_TOTAL": (49416393, 775000000),
            "RESOURCE_STALLS.ANY": (178886078, 775000000),
        }
        mem = {
            "L1D_PEND_MISS.PENDING": (702386810, 775000000),
            "CYCLE_ACTIVITY.STALLS_MEM_ANY": (764369035, 775000000),
            "CYCLE_ACTIVITY.STALLS_TOTAL": (764696125, 775000000),
            "RESOURCE_STALLS.ANY": (764489362, 775000000),
        }
        rep, suspects, valid = check_direction_and_suspects(comp, mem)
        self.assertFalse(valid)
        self.assertFalse(rep["L1D_PEND_MISS.PENDING"]["direction_ok"])
        self.assertTrue(any("within 5%" in s for s in suspects))

    def test_all_workloads_launch_signature(self):
        import inspect
        for name, cls in ALL_WORKLOADS.items():
            sig = inspect.signature(cls.launch)
            self.assertIn("launch_cpus", sig.parameters, f"{name}.launch missing launch_cpus")
            self.assertIn("sample_fn", sig.parameters, f"{name}.launch missing sample_fn")


if __name__ == "__main__":
    unittest.main(verbosity=2)
