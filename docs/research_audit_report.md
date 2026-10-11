# Research-Grade Implementation & Mathematical Audit Report: `hybridsched`

**Auditor Role**: Senior Computer Systems Researcher & Performance Evaluation Specialist  
**Target Architecture**: Intel Xeon w5-2565X (Sapphire Rapids-WS)  
**Audit Date**: 2026-10-11  

---

## Executive Summary

A comprehensive, line-by-line audit of the `hybridsched` codebase, test harnesses, and experimental artifacts was conducted. The audit revealed that while core scheduling state machines, sliding-window phase detectors, and Markov transition trackers are implemented in C, the experimental results reported in previous drafts were compromised by **critical measurement and platform discrepancies**.

Most notably:
1. **RAPL sysfs permissions (`EACCES`)** forced test harnesses to fall back to hardcoded static power multipliers (`30.5 W` for predictive, `32.0 W` for reactive, `46.0 W` for CFS). This completely fabricated the reported uniform ~33% EDP reduction vs CFS and ~4.6% predictive gain.
2. **The host processor is 100% homogeneous** (18 identical Golden Cove P-cores, 0 physical E-cores). Frequency clamping was disabled and unimplementable without root. Migration occurred solely between identical cores at 4.6 GHz.
3. **PMU sampling was blocked** by `kernel.perf_event_paranoid = 4`, forcing the daemon to read pre-cooked synthetic telemetry (`ipc=2.6/0.35`, `mpki=0.2/28.0`) injected by benchmarks via `/dev/shm`.
4. **The decision equation in code violates dimensional consistency**, directly subtracting milliseconds of migration penalty from percentage points of EDP reduction.

---

## Detailed Findings Matrix

| Finding ID | Component / File | Description | Severity | Impact on Claims |
| :--- | :--- | :--- | :--- | :--- |
| **AUD-01** | `harness/*.py` | Synthetic power table fallback masked missing RAPL permissions | **CRITICAL** | **INVALIDATES** all empirical energy and EDP claims. |
| **AUD-02** | `src/topology.c` | Host CPU is homogeneous; P/E pools are arbitrary affinity groups | **CRITICAL** | **INVALIDATES** claims of physical hybrid core migration. |
| **AUD-03** | `src/pmu_sampler.c` | `perf_event_open` failed (paranoid=4); fell back to `/dev/shm` synthetic constants | **CRITICAL** | **QUALIFIES** claims of live microarchitectural PMU tracking. |
| **AUD-04** | `src/phase_predictor.c` | Mathematical dimensional inconsistency in net EDP expected gain equation | **HIGH** | **REQUIRES REVISION** of analytic model and formulation. |
| **AUD-05** | `src/phase_predictor.c` | Triple-counting of cache warm-up and migration penalties | **HIGH** | Alters decision boundaries and sensitivity curves. |
| **AUD-06** | `src/pmu_sampler.c` | Group read format lacks enabled/running time scaling for counter multiplexing | **MEDIUM** | Potential undercounting on multiplexed PMU hardware. |
| **AUD-07** | `include/phase_predictor.h` | Inverted EMA $\beta$ smoothing notation between paper and implementation | **MEDIUM** | Notation discrepancy in manuscript. |
| **AUD-08** | `benchmarks/bench_service.c` | Missing tail latency (p95, p99) instrumentation for vector service queries | **LOW** | Limits service-level agreement (SLA) evaluation. |

---

## Detailed Findings & Technical Analysis

### AUD-01: Synthetic Energy Fallback Caused Fabricated Uniform Improvements
- **File & Function**: `harness/evaluate_ablation_and_break_even.py` (lines 105–125), `harness/evaluate_scheduler.py` (lines 86–102).
- **Exact Issue**:
  The Linux kernel RAPL file `/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj` is mode `0400` owned by `root:root`. In unprivileged execution, `read_rapl_uj()` caught `errno=13 (Permission denied)` and returned `None`.
  The evaluation harnesses silently caught `start_rapl is None` and executed the following fallback:
  ```python
  if policy_type == "static_p":
      power_w = 54.0
  elif policy_type == "static_e":
      power_w = 22.0
  elif policy_type == "predictive":
      power_w = 30.5
  elif policy_type == "reactive":
      power_w = 32.0
  else:
      power_w = 46.0
  energy_j = power_w * duration
  edp = energy_j * duration
  ```
  Because the benchmarks ran for a near-identical duration $T \approx 3.05$ s or $4.05$ s across policies:
  $$\frac{\text{EDP}_{\text{predictive}}}{\text{EDP}_{\text{CFS}}} = \frac{30.5 \times T^2}{46.0 \times T^2} = \frac{30.5}{46.0} \approx 0.6630 \implies 33.7\% \text{ reduction}$$
  $$\frac{\text{EDP}_{\text{reactive}} - \text{EDP}_{\text{predictive}}}{\text{EDP}_{\text{reactive}}} = \frac{32.0 - 30.5}{32.0} = \frac{1.5}{32.0} \approx 4.6875\% \text{ gain}$$
  This hard-coded calculation generated the suspiciously uniform ~33% and ~4.6% results across all granularities and workloads.
- **Severity**: **CRITICAL**.
- **Correction**:
  Remove all synthetic power fallbacks. Never report EDP unless physical energy counters are accessible. When energy counters are unavailable, report empirical wall-clock time, CPU seconds, IPC, and cache misses.

---

### AUD-02: Homogeneous Xeon Hardware Misrepresented as Hybrid P/E Cores
- **File & Function**: `src/topology.c` (`topology_init`), `config/daemon.conf`.
- **Exact Issue**:
  The system is an Intel Xeon w5-2565X with 18 identical Golden Cove Performance cores. Zero Gracemont/Crestmont E-cores exist. Frequency clamping was disabled (`enable_frequency_clamping = false`), was never implemented in the C code, and cannot be set without root permissions. CPUs 0–7 and 8–17 both run at up to 4.6 GHz with identical microarchitectures and shared L3 cache.
- **Severity**: **CRITICAL**.
- **Correction**:
  Cease claiming the evaluation runs on a hybrid CPU. Clearly state that the current experimental platform is a homogeneous partitioned system, and that evaluation on heterogeneous cores is future work.

---

### AUD-03: PMU Permission Failure (`paranoid=4`) and Benchmark Telemetry Injection
- **File & Function**: `src/pmu_sampler.c` (`pmu_init_thread_ctx`), `benchmarks/bench_*.c` (`update_shm_telemetry`).
- **Exact Issue**:
  On Ubuntu with `kernel.perf_event_paranoid = 4`, unprivileged processes cannot invoke `perf_event_open`.
  `pmu_init_thread_ctx` failed with `errno=13` and set `fallback_active = true`.
  In fallback mode, `pmu_sample_thread` read `/dev/shm/hybridsched_<pid>_<tid>`, which the benchmarks populated with hardcoded constants:
  `ipc = (phase == PHASE_COMPUTE) ? 2.6 : 0.35`
  `mpki = (phase == PHASE_COMPUTE) ? 0.2 : 28.0`
  The daemon was not reading hardware performance counters; it was reading synthetic constants written by the benchmark itself.
- **Severity**: **CRITICAL**.
- **Correction**:
  Document that PMU hardware tracking requires `kernel.perf_event_paranoid <= 1` or `CAP_PERFMON`. Transparently describe the `/dev/shm` IPC mechanism as a simulated telemetry conduit.

---

### AUD-04: Dimensional Inconsistency in Expected Net EDP Decision Model
- **File & Function**: `src/phase_predictor.c` (lines 178–184).
- **Exact Issue**:
  The reported paper equation is:
  $$\mathbb{E}[\Delta \text{EDP}] = P(S_{\text{next}}) \cdot \Delta \text{EDP}_{\text{benefit}}(\hat{\tau}_{\text{next}}) - C_{\text{mig}} - \gamma(1 - P(S_{\text{next}}))$$
  The C implementation computes:
  ```c
  double edp_delta_base = 35.0; // percentage points
  double uncertainty_penalty = 20.0 * (1.0 - dec.confidence); // percentage points
  double net_expected_gain = (dec.confidence * edp_delta_base) 
                             - pred->migration_cost_ms 
                             - uncertainty_penalty;
  ```
  1. **Units Mismatch**: `pred->migration_cost_ms` is in milliseconds (15.0 ms), whereas `edp_delta_base` is in percentage points (35.0%). Subtracting milliseconds from percentages is mathematically invalid.
  2. **Independence of Dwell Time**: `edp_delta_base` is a static constant `35.0` regardless of whether the upcoming phase is predicted to last 100 ms or 10,000 ms.
- **Severity**: **HIGH**.
- **Correction**:
  Reformulate the model with consistent dimensions:
  $$\Delta \text{EDP}_{\text{benefit}}(\hat{\tau}) = \hat{\tau} \cdot \left( \text{EDP}_{\text{unoptimal}} - \text{EDP}_{\text{optimal}} \right)$$
  $$C_{\text{mig}} = E_{\text{flush}} \cdot T_{\text{refill}} + \text{Syscall Overhead}$$
  Express all terms in energy-delay units ($J \cdot s$) or normalized dimensionless ratios.

---

### AUD-05: Triple-Counting of Migration Costs
- **File & Function**: `src/phase_predictor.c` (lines 161, 180), `src/scheduler_daemon.c` (line 277).
- **Exact Issue**:
  Migration penalties are applied in three distinct layers:
  1. Predictor Check 1: Veto if predicted target dwell $\hat{\tau}_{\text{next}} < \tau_{\min}$ (120 ms).
  2. Predictor Check 3: Subtraction of `migration_cost_ms` in `net_expected_gain`.
  3. Daemon State Machine: Veto if current residency dwell $< \tau_{\min}$.
- **Severity**: **HIGH**.
- **Correction**:
  Consolidate migration cost modeling into a unified decision function to avoid artificial suppression of valid phase migrations.

---

### AUD-06: Unscaled PMU Multiplexing in Group Format
- **File & Function**: `src/pmu_sampler.c` (lines 131–189).
- **Exact Issue**:
  `pe.read_format` sets `PERF_FORMAT_GROUP` but omits `PERF_FORMAT_TOTAL_TIME_ENABLED` and `PERF_FORMAT_TOTAL_TIME_RUNNING`. If counter multiplexing occurs, counter values will be undercounted.
- **Severity**: **MEDIUM**.
- **Correction**:
  Add `PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING` and scale delta counts by `time_enabled / time_running`.

---

### AUD-07: Inversion of EMA Smoothing Factor Notation
- **File & Function**: `include/phase_predictor.h` (`DEFAULT_DWELL_BETA = 0.70`).
- **Exact Issue**:
  The paper states $\beta = 0.30$. In code, line 78 uses:
  $$\hat{\tau} = \beta \cdot \hat{\tau}_{\text{prev}} + (1 - \beta) \cdot \tau_{\text{obs}}$$
  With $\beta = 0.70$, the weight of new observations is $1 - \beta = 0.30$.
- **Severity**: **MEDIUM**.
- **Correction**:
  Align mathematical documentation to define $\alpha = 0.30$ as the observation weight and $\beta = 0.70$ as historical retention.

---

## Required Action Plan for Publication Admissibility

1. **Purge Synthetic Data**: Remove all hardcoded wattage numbers from test scripts.
2. **Defensible Metric Scope**: Re-run all evaluations using verifiable empirical metrics:
   - Wall-clock execution time ($s$)
   - Thread CPU execution time ($s$)
   - Migration count and migration frequency ($s^{-1}$)
   - Controller tick latency ($\mu s$)
   - Prediction accuracy, false positive rate, and phase transition detection latency
3. **Transparent Hardware Disclosure**: Explicitly document that the evaluation was performed on a homogeneous Intel Xeon workstation using software affinity partitioning, and position true asymmetric evaluation as future work.
4. **Mathematical Alignment**: Correct the dimensional units and eliminate double-counting in `src/phase_predictor.c`.
