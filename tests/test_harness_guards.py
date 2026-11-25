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
from run_experiments import create_session_dir
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
