#!/usr/bin/env python3
"""
Unit tests for harness/plot_results.py using TemporaryDirectory only.
Never writes to results/.
"""

import os
import sys
import tempfile
import unittest

import pandas as pd

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "harness"))

from plot_results import (
    _load_real_rows,
    plot_edp_comparison,
    plot_probe_overhead,
    plot_speedup_vs_mpki,
    plot_telemetry_timeline,
)


class TestPlotResults(unittest.TestCase):
    def test_load_real_rows_excludes_invalid(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "runs.csv")
            pd.DataFrame([
                {"synthetic": "true",  "warmup": "false", "session_type": "evaluation", "session_valid": "true",  "row_valid": "true",  "v": 1},
                {"synthetic": "false", "warmup": "true",  "session_type": "evaluation", "session_valid": "true",  "row_valid": "true",  "v": 2},
                {"synthetic": "false", "warmup": "false", "session_type": "smoke",      "session_valid": "true",  "row_valid": "true",  "v": 3},
                {"synthetic": "false", "warmup": "false", "session_type": "evaluation", "session_valid": "false", "row_valid": "true",  "v": 4},
                {"synthetic": "false", "warmup": "false", "session_type": "evaluation", "session_valid": "true",  "row_valid": "false", "v": 5},
                {"synthetic": "false", "warmup": "false", "session_type": "evaluation", "session_valid": "true",  "row_valid": "true",  "v": 6},
            ]).to_csv(p, index=False)
            real = _load_real_rows(p, "test")
            self.assertEqual(len(real), 1)
            self.assertEqual(int(real.iloc[0]["v"]), 6)

    def test_plot_edp_skips_insufficient_and_renders_ok(self):
        with tempfile.TemporaryDirectory() as tmp:
            scsv = os.path.join(tmp, "summary.csv")
            out_png = os.path.join(tmp, "fig1.png")
            pd.DataFrame([
                {"workload_variant": "w1", "policy": "default-linux", "status": "OK", "mean": 10.0, "ci_lo": 9.0, "ci_hi": 11.0},
                {"workload_variant": "w1", "policy": "pacer-speedup", "status": "OK", "mean": 7.0, "ci_lo": 6.5, "ci_hi": 7.5},
                {"workload_variant": "w2", "policy": "default-linux", "status": "INSUFFICIENT", "mean": "", "ci_lo": "", "ci_hi": ""},
            ]).to_csv(scsv, index=False)
            ok = plot_edp_comparison(scsv, out_png)
            self.assertTrue(ok)
            self.assertTrue(os.path.exists(out_png) and os.path.getsize(out_png) > 0)

    def test_plot_telemetry_and_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            tcsv = os.path.join(tmp, "tel.csv")
            pd.DataFrame([
                {"timestamp_sec": 1.0, "ipc": 2.4, "mpki": 0.3, "core_pool": "P-Core"},
                {"timestamp_sec": 1.2, "ipc": 0.4, "mpki": 22.0, "core_pool": "E-Core"},
            ]).to_csv(tcsv, index=False)
            self.assertTrue(plot_telemetry_timeline(tcsv, os.path.join(tmp, "fig2.png")))
            self.assertTrue(plot_speedup_vs_mpki(tcsv, os.path.join(tmp, "fig3.png")))

            pcsv = os.path.join(tmp, "probe.csv")
            pd.DataFrame([
                {"status": "valid"}, {"status": "valid"}, {"status": "skipped"}, {"status": "invalidated"}
            ]).to_csv(pcsv, index=False)
            self.assertTrue(plot_probe_overhead(pcsv, os.path.join(tmp, "fig4.png")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
