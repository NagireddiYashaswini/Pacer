#!/usr/bin/env bash
# ==============================================================================
# Live Demonstration Script for hybridsched
# Demonstrates real-time phase change detection and dynamic core migration.
# ==============================================================================
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${CYAN}======================================================================${NC}"
echo -e "${CYAN}   LIVE DEMO: Measured-Speedup-Aware Hybrid CPU Scheduler (hybridsched) ${NC}"
echo -e "${CYAN}======================================================================${NC}"

# Ensure binaries are built
make all > /dev/null

echo -e "\n${BLUE}[1/3] Launching Adaptive Scheduler Daemon in background...${NC}"
./bin/hybridsched_daemon --interval 150 --cooldown 350 --hysteresis 1 > results/demo_daemon.log 2>&1 &
DAEMON_PID=$!
echo -e "      Daemon PID: ${GREEN}$DAEMON_PID${NC} (Sampling tick: 150ms, Cooldown: 350ms)"

# Give daemon time to initialize
sleep 0.5

echo -e "\n${BLUE}[2/3] Launching Dynamic Phase-Shifter Workload (bench_phase)...${NC}"
echo -e "      Alternating between Compute (AVX2 GEMM) and Memory (Pointer Chase)..."
echo -e "      ------------------------------------------------------------------"

# Run bench_phase live
./bin/bench_phase --phases 4 --phase-duration 1.2

echo -e "      ------------------------------------------------------------------"

# Clean up daemon
echo -e "\n${BLUE}[3/3] Shutting down daemon and extracting migration timeline...${NC}"
kill -INT $DAEMON_PID 2>/dev/null || true
wait $DAEMON_PID 2>/dev/null || true

echo -e "\n${PURPLE}====================== REAL-TIME MIGRATION LOG ======================${NC}"
grep -E "MIGRATION|THREAD ATTACH" results/demo_daemon.log || cat results/demo_daemon.log
echo -e "${PURPLE}=====================================================================${NC}"

echo -e "\n${GREEN}[✓] Live Demonstration Complete! Telemetry logged to results/daemon_telemetry.csv${NC}\n"
