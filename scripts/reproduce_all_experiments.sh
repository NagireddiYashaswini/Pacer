#!/usr/bin/env bash
# ==============================================================================
# 1-Click Automated Reproduction Script
# Predictive, Energy-Aware Scheduling on Hybrid CPUs
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

echo "======================================================================"
echo "  Predictive Hybrid CPU Scheduling: 1-Click Reproduction Pipeline     "
echo "======================================================================"
echo "[+] Working Directory: ${ROOT_DIR}"
echo "[+] Host: $(uname -n) | Kernel: $(uname -r)"

# Step 1: Clean and Recompile Everything
echo ""
echo ">>> [1/4] Compiling Binaries, Benchmarks, and Tests..."
make clean
make all
echo "[+] Build completed successfully."

# Step 2: Run Predictor and Phase Detector Unit Tests
echo ""
echo ">>> [2/4] Running Unit Tests..."
./bin/test_phase_detector
./bin/test_predictor
echo "[+] All mathematical unit tests verified!"

# Step 3: Run Stage 4 Empirical Evaluation Suite
echo ""
echo ">>> [3/4] Running Stage 4 Controlled Evaluation & Ablation Suite..."
python3 harness/evaluate_ablation_and_break_even.py --trials 3
echo "[+] Empirical evaluation completed."

# Step 4: Generate Publication Figures
echo ""
echo ">>> [4/4] Generating Publication Figures..."
python3 scripts/plot_paper_figures.py
echo "[+] Figures rendered in results/figures/."

echo ""
echo "======================================================================"
echo "  REPRODUCTION COMPLETE! All experimental artifacts generated:      "
echo "======================================================================"
echo "  1. Paper Draft:        docs/research_paper_draft.md"
echo "  2. Evaluation Report:  results/stage4_evaluation_report.md"
echo "  3. Granularity Data:   results/stage4_granularity_ablation.csv"
echo "  4. Workload Data:      results/stage4_workloads_comparison.csv"
echo "  5. Contention Data:    results/stage4_contention_results.csv"
echo "  6. Publication Figs:   results/figures/fig1_break_even_granularity.png"
echo "                         results/figures/fig2_reactive_vs_predictive_gain.png"
echo "                         results/figures/fig3_contention_and_workloads.png"
echo "                         results/figures/fig4_daemon_overhead_distribution.png"
echo "======================================================================"
