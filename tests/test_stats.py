#!/usr/bin/env python3
"""
Unit tests for statistical functions in harness/analyze.py using
hand-computable inputs with known answers. Never writes to results/.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "harness"))

from analyze import (
    bootstrap_ci,
    holm_bonferroni,
    matched_pairs_rank_biserial,
    paired_wilcoxon_and_effect,
)


class TestStatsMath(unittest.TestCase):
    def test_holm_bonferroni_known(self):
        # Raw p-values: [0.01, 0.04, 0.03] -> sorted: 0.01 (x3=0.03), 0.03 (x2=0.06), 0.04 (x1=0.04 -> monotone max 0.06)
        adj = holm_bonferroni([0.01, 0.04, 0.03])
        self.assertAlmostEqual(adj[0], 0.03, places=9)
        self.assertAlmostEqual(adj[1], 0.06, places=9)
        self.assertAlmostEqual(adj[2], 0.06, places=9)

    def test_rank_biserial_all_wins(self):
        # All negative diffs (policy < baseline => all wins for lower-is-better)
        # W+ = 0, W- = 1+2+3+4 = 10 => r = (0 - 10)/10 = -1.0
        r = matched_pairs_rank_biserial([-1.0, -2.0, -3.0, -4.0])
        self.assertAlmostEqual(r, -1.0, places=9)

    def test_rank_biserial_all_losses(self):
        r = matched_pairs_rank_biserial([1.0, 2.0, 3.0])
        self.assertAlmostEqual(r, 1.0, places=9)

    def test_paired_wilcoxon_and_effect_known(self):
        # 10 pairs: baseline = 10.0, policy = 8.0 (-20% every pair)
        base = [10.0] * 10
        pol = [8.0] * 10
        res = paired_wilcoxon_and_effect(pol, base, n_resamples=1000, seed=42)
        self.assertEqual(res["n_pairs"], 10)
        self.assertEqual(res["wins"], 10)
        self.assertEqual(res["ties"], 0)
        self.assertEqual(res["losses"], 0)
        self.assertAlmostEqual(res["pct_change_mean"], -20.0, places=6)
        self.assertAlmostEqual(res["pct_change_ci_lo"], -20.0, places=6)
        self.assertAlmostEqual(res["pct_change_ci_hi"], -20.0, places=6)
        self.assertAlmostEqual(res["rank_biserial_r"], -1.0, places=6)
        self.assertLess(res["p_value"], 0.01)

    def test_bootstrap_ci_constant(self):
        lo, hi = bootstrap_ci([5.0, 5.0, 5.0, 5.0], n_resamples=500, seed=1)
        self.assertAlmostEqual(lo, 5.0, places=9)
        self.assertAlmostEqual(hi, 5.0, places=9)


if __name__ == "__main__":
    unittest.main(verbosity=2)
