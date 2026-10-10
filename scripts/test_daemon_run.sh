#!/usr/bin/env bash
set -e

mkdir -p results
echo "=== Starting Hybrid Scheduler Daemon ==="
./bin/hybridsched_daemon --interval 200 --cooldown 400 --hysteresis 1 > results/daemon_test.log 2>&1 &
DAEMON_PID=$!
echo "Daemon PID: $DAEMON_PID"

sleep 0.5
echo "=== Starting Bench Phase (4 phases x 1.0s) ==="
./bin/bench_phase --phases 4 --phase-duration 1.0

sleep 0.5
echo "=== Stopping Daemon ==="
kill -INT $DAEMON_PID 2>/dev/null || true
wait $DAEMON_PID 2>/dev/null || true

echo "=== Daemon Log Output ==="
cat results/daemon_test.log
