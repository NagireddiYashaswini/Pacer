# Measured-Speedup-Aware CPU Scheduling on Hybrid x86 Architectures

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![Platform](https://img.shields.io/badge/platform-Linux%20x86__64-blue.svg)]()
[![Language](https://img.shields.io/badge/language-C99%20%7C%20Python%203-orange.svg)]()

A production-grade, user-space adaptive CPU scheduling daemon (`hybridsched`) designed for heterogeneous x86 processors (Performance Cores vs. Efficient Cores). `hybridsched` continuously samples hardware performance counters via `perf_event_open`, identifies dynamic microarchitectural phase shifts, mitigates memory-stall penalties, and dynamically adjusts thread affinity masks (`sched_setaffinity`) to maximize throughput and minimize the Energy-Delay Product ($EDP$).

- **Host Execution Requirement**: `harness/env_check.py` and `harness/run_experiments.py` enforce `harness/host_guard.py`, refusing to run unless executed from the host init namespace (`/proc/1/comm` is `systemd`/`init` and `/proc/1/cgroup` is the host root cgroup) and pinned to `control_plane_cpus` from `config/pools.yaml` (e.g., `taskset -c 17,35`).

---

## Key Features

- **Microarchitectural Telemetry Engine**: Atomic sampling of hardware counters (cycles, instructions, LLC misses) via `PERF_FORMAT_GROUP`. Computes real-time Instructions Per Cycle ($IPC$) and LLC Misses Per 1,000 Instructions ($MPKI$).
- **Sliding-Window Phase Detector**: $O(1)$ ring buffer calculating rolling IPC mean ($\mu_{IPC}$) and rolling variance ($\sigma^2_{IPC}$) to distinguish transient fluctuations from sustained phase shifts.
- **Memory-Stall Penalty Awareness**: Threads experiencing high memory stalls ($MPKI \ge 15.0$) are steered to E-cores, preventing stalled threads from tying up wide, power-hungry P-core execution units.
- **Anti-Thrashing State Machine**: Enforces configurable hysteresis thresholds ($H \ge 2$) and residency cooldown timers ($T_{cooldown} \ge 400$ ms) to eliminate migration thrashing.
- **Intel RAPL Energy Telemetry**: Direct reading of processor microjoules via `/sys/class/powercap/intel-rapl` with fallback power estimation.
- **Multithreaded TID Support**: Automatically discovers and steers individual worker threads (`/proc/<pid>/task/`) and active workloads via zero-copy `/dev/shm` IPC.
- **Hardware Topology & Emulation Support**: Native detection of Intel hybrid PMUs with support for configurable core partitioning and frequency clamping on homogeneous hosts.

---

## Evaluation Workflow (Host Terminal)

> **Important:** Real evaluations must be executed from a host terminal pinned to the control-plane CPUs (`taskset -c 17,35` per `config/pools.yaml`). `harness/host_guard.py` enforces host PID/cgroup namespaces and control-plane affinity.

1. **Verify host environment:**
   ```bash
   taskset -c 17,35 ./scripts/verify_env.sh
   ```
2. **Run a smoke session (`results/smoke/<session_id>/`):**
   ```bash
   taskset -c 17,35 python3 harness/run_experiments.py --smoke --session-id smoke_01
   ```
3. **Run a full evaluation session (`results/raw/<session_id>/`):**
   ```bash
   taskset -c 17,35 python3 harness/run_experiments.py --config config/experiment.yaml --session-id eval_01
   ```
4. **Analyze results & generate LaTeX tables + figures:**
   ```bash
   python3 harness/analyze.py
   python3 harness/plot_results.py
   ```
   Outputs: `results/analysis_summary.csv`, `results/analysis_pairwise.csv`, `results/table_summary.tex`, `results/table_pairwise.tex`, and `results/fig1_edp_comparison.png`–`fig4_probe_overhead.png`.

---

## Directory Structure

```
.
├── Makefile                     # Top-level build configuration
├── README.md                    # Project overview and reproduction guide
├── benchmarks/                  # Deterministic microbenchmarks
│   ├── bench_compute.c          # AVX2 8x8 tiled GEMM compute kernel
│   ├── bench_memory.c           # 64MB pseudo-random pointer-chasing kernel
│   └── bench_phase.c            # Alternating multi-phase benchmark
├── bin/                         # Compiled binaries
├── config/
│   └── daemon.conf              # Daemon runtime configuration
├── harness/                     # Benchmarking and plotting tools
│   ├── evaluate_scheduler.py    # Master end-to-end evaluation suite
│   ├── plot_results.py          # Publication-grade plotting script
│   └── run_benchmarks.py        # Baseline profiling harness
├── include/                     # C header files
├── results/                     # Generated CSV logs and high-res figures
│   ├── fig1_edp_comparison.png
│   ├── fig2_telemetry_timeline.png
│   └── fig3_speedup_vs_mpki.png
├── scripts/                     # Operational scripts
│   ├── demo_live.sh             # 1-click live demo script
│   ├── setup_env.sh             # Frequency scaling and permissions setup
│   └── test_daemon_run.sh       # Verification test script
└── src/                         # Core scheduler source code
    ├── daemon_main.c            # Daemon CLI and main loop
    ├── phase_detector.c         # Sliding-window statistical variance detector
    ├── pmu_sampler.c            # perf_event_open group sampler and TID parser
    ├── rapl_energy.c            # Intel RAPL reader and power model
    ├── scheduler_daemon.c       # Anti-thrashing decision engine
    └── topology.c               # Sysfs CPU topology parser
```

---

## Quick Start & Reproduction

### 1. Build All Binaries
```bash
make clean && make all
```

### 2. Run the 1-Click Live Demonstration
```bash
./scripts/demo_live.sh
```
This script launches the daemon, runs an alternating phase workload (`bench_phase`), and prints the real-time core migration events to the terminal.

### 3. Run Unit Tests
```bash
make test
```

### 4. Generate Publication-Quality Visualizations
```bash
python3 harness/plot_results.py
```
The figures will be saved in `results/`:
- `results/fig1_edp_comparison.png`
- `results/fig2_telemetry_timeline.png`
- `results/fig3_speedup_vs_mpki.png`
- `results/fig4_probe_overhead.png`
- `results/fig2_telemetry_timeline.png`
- `results/fig3_speedup_vs_mpki.png`
