# Experimental Methodology & Measurement Integrity Audit

**Auditor Role**: Senior Computer Systems Researcher & Performance Specialist  
**Target Repository**: `hybridsched` (`https://github.com/NagireddiYashaswini/Pacer`)  
**Audit Date**: 2026-10-11  

---

## 1. System Baseline Configuration

Rigorous systems performance evaluation requires complete disclosure of host operating parameters. The baseline state of the experimental platform was audited as follows:

| Subsystem / Setting | Value / Configuration | Measurement Impact |
| :--- | :--- | :--- |
| **Operating System** | Linux 6.8.0-45-generic x86_64 (Ubuntu 24.04 LTS) | Standard server kernel distribution |
| **Processor** | Intel Xeon w5-2565X (18 Physical Cores, 36 Threads) | Workstation Sapphire Rapids platform |
| **CPU Scaling Driver** | `intel_pstate` | Hardware-managed P-states (HWP) active |
| **Scaling Governor** | `powersave` (default for `intel_pstate`) | Energy-Performance Preference (EPP) driven |
| **Frequency Limits** | Min: 800,000 kHz (0.8 GHz) \| Max: 4,600,000 kHz (4.6 GHz) | Shared identically across all CPUs (0–35) |
| **Frequency Clamping** | **DISABLED** (`enable_frequency_clamping = false`) | Zero frequency asymmetry between core pools |
| **Perf Paranoid Level** | `kernel.perf_event_paranoid = 4` | **Blocks unprivileged PMU hardware sampling** |
| **RAPL Interface Permissions**| `-r-------- (0400 root:root)` | **Blocks unprivileged energy readings** |

---

## 2. Workload Realism & Kernel Audit

The benchmark suite consists of five distinct microbenchmarks:

### 2.1 `bench_compute` (AVX2 GEMM)
- **Implementation**: Single-precision matrix multiplication ($C = A \times B$) using AVX2 fused multiply-add (`_mm256_fmadd_ps`) with $8 \times 8$ register unrolling over $128 \times 128$ matrices.
- **Microarchitectural Signature**: High arithmetic intensity, IPC $\approx 2.4 - 2.7$, MPKI $< 0.5$, zero branch mispredicts.
- **Authenticity**: **GENUINE**. Highly compute-bound execution kernel.

### 2.2 `bench_memory` (Pointer Chase)
- **Implementation**: Circular pointer chain allocated over a 64 MiB buffer (exceeding the 37.5 MiB host L3 cache), shuffled via Fisher-Yates with a 64-bit linear congruential PRNG to defeat hardware stream prefetchers. Dereferences 16 dependent pointers per loop iteration.
- **Microarchitectural Signature**: Severe memory stall bound, IPC $\approx 0.30 - 0.45$, MPKI $\approx 25 - 30$.
- **Authenticity**: **GENUINE**. Defeats prefetchers and stresses memory controllers.

### 2.3 `bench_phase` (Synthetic Multi-Phase)
- **Implementation**: Alternates between the compute GEMM kernel and the memory pointer-chasing kernel for fixed duration intervals (e.g., 100 ms to 2,000 ms).
- **Caveat**: When `perf_event_open` fails, the benchmark writes pre-cooked metrics to `/dev/shm/hybridsched_<pid>_<tid>` (`ipc=2.6` or `0.35`, `mpki=0.2` or `28.0`).

### 2.4 `bench_irregular` (Stochastic Markov Transitions)
- **Implementation**: Simulates phase changes governed by a stochastic transition matrix with dwell durations uniformly sampled between $\tau_{\min}$ (250 ms) and $\tau_{\max}$ (750 ms).
- **Caveat**: Also writes pre-cooked telemetry to `/dev/shm` to drive unprivileged daemon phase tracking.

### 2.5 `bench_service` (Vector Search Service)
- **Implementation**: Multi-threaded vector scoring service calculating AVX2 cosine similarity against an 80,000-vector index (128 dimensions). Interleaves memory-intensive index scanning with compute-bound dot product operations.
- **Caveat**: Also writes pre-cooked telemetry to `/dev/shm`.

---

## 3. Energy Measurement Integrity Audit

### 3.1 The Intel RAPL Interface
Intel Running Average Power Limit (RAPL) energy counters are exposed by Linux at:
```text
/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj
```
This node provides a 32-bit or 64-bit cumulative microjoule register that updates every $\approx 1$ ms.

### 3.2 Security Permissions & Failure Mode
On modern Linux kernels, reading RAPL counters exposes microarchitectural side-channels (e.g., Platypus attack), leading distributions to restrict the file mode to `0400` (`root:root`).
In all test harness executions:
```python
start_rapl = read_rapl_uj()  # Returns None due to EACCES (errno 13)
```

### 3.3 The Fallback Artifact
Because `start_rapl is None`, test harnesses fell back to static wattage multipliers:
- Static P: $54.0$ W
- Default CFS: $46.0$ W
- Reactive Scheduler: $32.0$ W
- Predictive Scheduler: $30.5$ W
- Static E: $22.0$ W

**Scientific Verdict**:
Energy ($J$) was calculated as $P_{\text{table}} \times T$, and EDP ($J \cdot s$) as $P_{\text{table}} \times T^2$.
Because run durations were identical within milliseconds, the reported ~33% EDP reduction vs CFS and ~4.6% gain over reactive scheduling were **100% artifacts of these hard-coded constants**.

---

## 4. What Is Verifiable vs. What Was Simulated

| Metric / Phenomenon | Status on Audited Testbed | Justification |
| :--- | :--- | :--- |
| **Physical Core Heterogeneity** | **NOT VERIFIABLE** | Host has 18 identical P-cores. Core pools are software affinity groups. |
| **Physical Energy (Joules)** | **NOT VERIFIABLE** | RAPL sysfs requires root privileges; unprivileged reads failed. |
| **Physical EDP ($J \cdot s$)** | **NOT VERIFIABLE** | Direct product of unmeasured energy. |
| **Wall-Clock Execution Time ($s$)** | **VERIFIED** | Measured with `time.perf_counter()` and `clock_gettime(CLOCK_MONOTONIC_RAW)`. |
| **Thread CPU Seconds ($s$)** | **VERIFIED** | Measured via `getrusage()` / `/proc/[pid]/task/[tid]/stat`. |
| **Thread Migration Counts** | **VERIFIED** | Captured via scheduler logs and `sched_setaffinity` return codes. |
| **Controller Tick Overhead ($\mu s$)** | **VERIFIED** | Measured around daemon tick loop ($\approx 8 - 14\,\mu s$). |
| **Phase Transition Accuracy** | **VERIFIED (ON SHM)** | Evaluated on synthetic phase-change sequences via `/dev/shm`. |

---

## 5. Statistical Rigor Protocol

In subsequent evaluations:
1. **No Simulated Fallbacks**: If physical energy is unreadable, report energy as `N/A` or unverified. Do not synthesize power values.
2. **Replication**: All benchmarks must be run across $N \ge 5$ independent trials.
3. **Statistical Reporting**: Report both sample mean ($\mu$) and sample standard deviation ($\sigma$).
4. **Warm-up**: Discard the first trial to eliminate cold-start cache filling and initial file page caching effects.
5. **Separation of Workloads**: Reset system state, terminate lingering background processes, and sleep 2 seconds between trials to maintain thermal stability.
