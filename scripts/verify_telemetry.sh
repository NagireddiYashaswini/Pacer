#!/usr/bin/env bash
# ==============================================================================
# hybridsched — Post-Blocker Telemetry Verification Script
# Verifies real (non-synthetic) IPC, MPKI, and RAPL energy readings.
# Run AFTER scripts/fix_system_blockers.sh
# Usage: bash scripts/verify_telemetry.sh
# ==============================================================================
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; MAGENTA='\033[0;35m'; BOLD='\033[1m'; NC='\033[0m'

PASS=0; FAIL=0; WARN=0
check() { local label="$1" val="$2" expect="$3"
    if echo "$val" | grep -qE "$expect" 2>/dev/null; then
        echo -e "  ${GREEN}[PASS]${NC} ${label}: ${val}"; ((PASS++))
    else
        echo -e "  ${RED}[FAIL]${NC} ${label}: ${val} (expected: ${expect})"; ((FAIL++))
    fi
}

echo -e "${CYAN}${BOLD}"
echo "======================================================================"
echo "  hybridsched — Telemetry Verification Suite"
echo "  $(date)  |  Host: $(hostname)  |  Kernel: $(uname -r)"
echo -e "======================================================================${NC}"

mkdir -p results

# ============================================================
# CHECK 1: perf_event_paranoid level
# ============================================================
echo -e "\n${BOLD}[1/5] PMU kernel.perf_event_paranoid${NC}"
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "error")
check "perf_event_paranoid" "$PARANOID" "^-1$"

# ============================================================
# CHECK 2: RAPL sysfs read access (non-root)
# ============================================================
echo -e "\n${BOLD}[2/5] Intel RAPL sysfs non-root readability${NC}"
for node in \
    "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj" \
    "/sys/class/powercap/intel-rapl/intel-rapl:0:0/energy_uj"
do
    if [ -f "$node" ]; then
        VAL=$(cat "$node" 2>/dev/null || echo "EACCES")
        MODE=$(stat -c "%a" "$node" 2>/dev/null || echo "?")
        if [ "$VAL" != "EACCES" ] && [ -n "$VAL" ]; then
            echo -e "  ${GREEN}[PASS]${NC} ${node} (mode=${MODE}): ${VAL} µJ"; ((PASS++))
        else
            echo -e "  ${RED}[FAIL]${NC} ${node} (mode=${MODE}): permission denied or empty"; ((FAIL++))
        fi
    else
        echo -e "  ${YELLOW}[WARN]${NC} ${node}: file not found (domain may not exist on this CPU)"; ((WARN++))
    fi
done

# ============================================================
# CHECK 3: perf_event_open syscall — sanity test via perf stat
# ============================================================
echo -e "\n${BOLD}[3/5] perf_event_open syscall reachability${NC}"
if command -v perf &>/dev/null; then
    PERF_OUT=$(perf stat -e cycles,instructions,cache-misses -- sleep 0.1 2>&1 || true)
    if echo "$PERF_OUT" | grep -q "cycles"; then
        echo -e "  ${GREEN}[PASS]${NC} perf stat cycles/instructions/LLC-misses counters accessible"
        echo "$PERF_OUT" | grep -E "cycles|instructions|cache-misses" | head -5 | sed 's/^/         /'
        ((PASS++))
    else
        echo -e "  ${RED}[FAIL]${NC} perf stat failed or returned no hardware counters"
        echo "$PERF_OUT" | head -6 | sed 's/^/         /'
        ((FAIL++))
    fi
else
    echo -e "  ${YELLOW}[WARN]${NC} 'perf' binary not found; skipping perf stat check"; ((WARN++))
fi

# ============================================================
# CHECK 4: Live daemon run (5-second trial, real PMU telemetry)
# ============================================================
echo -e "\n${BOLD}[4/5] Live daemon trial run — 5-second window w/ bench_phase${NC}"

if [ ! -x ./bin/hybridsched_daemon ] || [ ! -x ./bin/bench_phase ]; then
    echo -e "  ${YELLOW}[WARN]${NC} Binaries not found. Running make first..."
    make all > /dev/null 2>&1
fi

TELEM_FILE="results/verify_telemetry_live.csv"
DAEMON_LOG="results/verify_daemon.log"
> "$TELEM_FILE"
> "$DAEMON_LOG"

# Patch daemon to write to our verification CSV by symlinking
ln -sf "$(realpath $TELEM_FILE)" results/daemon_telemetry.csv 2>/dev/null || \
    cp /dev/null results/daemon_telemetry.csv

echo "  Launching daemon (interval=200ms, cooldown=400ms, hysteresis=2)..."
./bin/hybridsched_daemon \
    --interval 200 \
    --cooldown 400 \
    --hysteresis 2 \
    > "$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
echo -e "  Daemon PID: ${MAGENTA}${DAEMON_PID}${NC}"

sleep 0.8

echo "  Launching bench_phase (6 phases × 0.8s = 4.8s)..."
./bin/bench_phase --phases 6 --phase-duration 0.8 &
BENCH_PID=$!

# Let both run for 5 seconds
sleep 5.5

# Collect before kill
TELEM_LINES=$(wc -l < results/daemon_telemetry.csv 2>/dev/null || echo 0)

kill -INT $DAEMON_PID 2>/dev/null || true
wait $BENCH_PID 2>/dev/null || true
wait $DAEMON_PID 2>/dev/null || true

echo ""
echo "  --- Daemon log (migration + PMU path) ---"
grep -E "MIGRATION|THREAD ATTACH|PMU|RAPL|fallback|Predictor Telemetry|Daemon profiling" \
    "$DAEMON_LOG" 2>/dev/null | head -20 | sed 's/^/  /'
echo "  ---"

# Verify telemetry CSV has real data
echo ""
echo -e "  Telemetry CSV rows written: ${TELEM_LINES}"
if [ "$TELEM_LINES" -gt 2 ]; then
    echo -e "  ${GREEN}[PASS]${NC} Telemetry CSV populated (${TELEM_LINES} rows)"
    echo "  Sample rows:"
    head -4 results/daemon_telemetry.csv | sed 's/^/    /'
    ((PASS++))
else
    echo -e "  ${RED}[FAIL]${NC} Telemetry CSV has ≤2 rows — daemon may not have attached workload"
    ((FAIL++))
fi

# Check for real PMU values (non-synthetic: IPC should vary, not fixed at 1.5)
echo ""
echo "  Checking for real (non-synthetic) IPC/MPKI signal..."
if command -v python3 &>/dev/null && [ "$TELEM_LINES" -gt 2 ]; then
    python3 - <<'PYEOF'
import csv, sys
rows = []
try:
    with open("results/daemon_telemetry.csv") as f:
        reader = csv.DictReader(f)
        rows = [r for r in reader if r.get("ipc","0") != "0"]
except Exception as e:
    print(f"  [WARN] Could not parse CSV: {e}")
    sys.exit(0)

if not rows:
    print("  [FAIL] No non-zero IPC rows in CSV")
    sys.exit(0)

ipcs  = [float(r["ipc"])  for r in rows if r["ipc"]]
mpkis = [float(r["mpki"]) for r in rows if r["mpki"]]

ipc_min,  ipc_max  = min(ipcs),  max(ipcs)
mpki_min, mpki_max = min(mpkis), max(mpkis)
ipc_range = ipc_max - ipc_min
mpki_range = mpki_max - mpki_min

print(f"  IPC  range: {ipc_min:.3f} – {ipc_max:.3f}  (spread={ipc_range:.3f})")
print(f"  MPKI range: {mpki_min:.2f}  – {mpki_max:.2f}  (spread={mpki_range:.2f})")

# Synthetic fallback produces constant IPC≈1.5. Real PMU shows variation.
if ipc_range > 0.5 and mpki_range > 5.0:
    print("  [PASS] High IPC/MPKI variance → REAL hardware PMU counters active")
elif ipc_range > 0.1:
    print("  [WARN] Moderate IPC variance → possibly SHM telemetry (bench_phase exposes counters)")
else:
    print("  [FAIL] Near-zero IPC variance → synthetic fallback still active (check perf paranoid)")
PYEOF
fi

# ============================================================
# CHECK 5: RAPL delta energy (confirm counter is live/moving)
# ============================================================
echo -e "\n${BOLD}[5/5] RAPL energy counter liveness (delta across 2s)${NC}"
RAPL_PKG="/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj"
if [ -f "$RAPL_PKG" ]; then
    E1=$(cat "$RAPL_PKG" 2>/dev/null || echo 0)
    sleep 2
    E2=$(cat "$RAPL_PKG" 2>/dev/null || echo 0)
    DELTA=$(( E2 - E1 ))
    if [ "$DELTA" -gt 1000000 ]; then   # >1 J in 2s = plausible
        echo -e "  ${GREEN}[PASS]${NC} RAPL energy_uj is live: E1=${E1} → E2=${E2} (Δ=${DELTA} µJ ≈ $(( DELTA / 2000000 )) W avg)"
        ((PASS++))
    elif [ "$DELTA" -gt 0 ]; then
        echo -e "  ${YELLOW}[WARN]${NC} RAPL counter incremented by ${DELTA} µJ (may be throttled or C-state-heavy)"
        ((WARN++))
    else
        echo -e "  ${RED}[FAIL]${NC} RAPL counter did not increment (Δ=0) — hardware may have disabled it"
        ((FAIL++))
    fi
else
    echo -e "  ${YELLOW}[WARN]${NC} RAPL sysfs node not found"; ((WARN++))
fi

# ============================================================
# FINAL SCORECARD
# ============================================================
TOTAL=$(( PASS + FAIL + WARN ))
echo ""
echo -e "${CYAN}${BOLD}======================================================================"
echo "  VERIFICATION SCORECARD"
echo -e "======================================================================${NC}"
echo -e "  ${GREEN}PASS: ${PASS}${NC}  |  ${RED}FAIL: ${FAIL}${NC}  |  ${YELLOW}WARN: ${WARN}${NC}  |  TOTAL: ${TOTAL}"
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo -e "  ${GREEN}${BOLD}✓ All critical checks passed. Real hardware telemetry is operational.${NC}"
    echo -e "  ${GREEN}  You may now run: python3 harness/evaluate_scheduler.py${NC}"
    echo -e "  ${GREEN}                   python3 harness/plot_results.py${NC}"
else
    echo -e "  ${RED}${BOLD}✗ ${FAIL} check(s) failed. Review the log above.${NC}"
    echo "    Common fix: sudo bash scripts/fix_system_blockers.sh"
fi
echo ""
echo "  Full daemon log: ${DAEMON_LOG}"
echo "  Telemetry CSV : results/daemon_telemetry.csv"
