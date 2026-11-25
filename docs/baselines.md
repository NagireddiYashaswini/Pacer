# Baselines, Emulated Heterogeneity, and Evaluation Split

## 1. Host Environment & `default-linux` Semantics

The evaluation host (`Intel(R) Xeon(R) w5-2565X`) is a **homogeneous** 18-core
(36-thread) processor with no `cpu_core`/`cpu_atom` hybrid PMUs. Heterogeneity
is **emulated** via `config/pools.yaml`:
- **P-core pool:** logical CPUs `0-7` (physical cores `0-7`), turbo OFF (`no_turbo=1`).
- **E-core pool:** logical CPUs `8-17` (physical cores `8-17`), clamped via
  `cpufreq/scaling_max_freq` to `2000000` kHz (along with their SMT siblings `26-35`).
- **Excluded siblings:** logical CPUs `18-25` (SMT siblings of the P-core pool)
  are kept idle and never used by the daemon, workloads, or harness.

Because this is **not** a native Intel hybrid CPU, `default-linux` in
`harness/policies.py` represents the stock heterogeneity-unaware Linux CFS
scheduler restricted via `taskset` to the allowed CPU set (`p_cores + e_cores`,
never `18-25` or `26-35`). **It does NOT include Intel Thread Director (ITD)
hardware feedback.** Any comparison against `default-linux` must explicitly note
that it evaluates stock CFS under emulated frequency heterogeneity.

**Host Execution Guard (`harness/host_guard.py`):** Both `harness/env_check.py`
and `harness/run_experiments.py` refuse to run unless:
1. `PID 1` is the host init (`systemd` or `init` in `/proc/1/comm`).
2. `Cpus_allowed_list` in `/proc/self/status` is a subset of `control_plane_cpus`
   from `config/pools.yaml` (launch via `taskset -c 17,35 ...`).
3. `/proc/1/cgroup` shows the host root cgroup (`/init.scope` or `/`).

---

## 2. Policy Registry (`harness/policies.py`)

| Policy Name | Deployable? | Mechanism |
|---|---|---|
| `default-linux` | Yes (baseline) | Unpinned CFS restricted to `p_cores + e_cores`; records kernel version. |
| `static-P` | Yes (baseline) | Pins all workload threads to `p_cores` (`config/pools.yaml`). |
| `static-E` | Yes (baseline) | Pins all workload threads to `e_cores` (`config/pools.yaml`). |
| `oracle-static` | **NO (OFFLINE UPPER BOUND)** | Offline profiling runs each thread/process on both pools in isolation, measures wall time and RAPL energy, computes EDP per pool, and saves to `results/profiles/` (never overwriting). Replay mode pins each thread to its lower-EDP pool. **Not deployable**; represents an offline upper bound for static assignment. |
| `pie-like` | Yes (baseline) | Simplified Performance Impact Estimation (**reimplementation inspired by Van Craeynest et al., ISCA 2012**). See §3. |
| `hass-like` | Yes (offline-assisted) | Static signature-based assignment (**reimplementation inspired by Shelepov et al., 2009**). See §4. |
| `pacer-mpki` | Yes (ablation) | Runs `hybridsched_daemon --policy mpki`. |
| `pacer-speedup` | Yes (proposed) | Runs `hybridsched_daemon --policy speedup`. |

---

## 3. `pie-like` (Reimplementation Inspired by Van Craeynest et al., ISCA 2012)

> **Disclaimer:** This is **NOT** the original PIE implementation. It is a
> simplified reimplementation inspired by Van Craeynest et al., *"Scheduling
> Heterogeneous Multi-Cores through Performance Impact Estimation (PIE)"*
> (ISCA 2012).

- **What the original paper did:**
  - PIE estimates the performance of a workload on big vs. small cores using
    hardware performance counters for **Memory-Level Parallelism (MLP)**
    (outstanding memory requests / L1D miss pending cycles) and
    **Instruction-Level Parallelism (ILP)** / CPI-stack stall cycles, predicting
    core-type speedup without migrating threads across core types.
  - TODO: verify exact formula coefficients and microarchitectural counter names
    against Van Craeynest et al. (ISCA 2012) Section 3–4.
- **What our `pie-like` implementation does and why:**
  - At setup, `PieLikePolicy.setup()` probes all four raw Intel PMU event
    encodings directly via `perf_event_open(PERF_TYPE_RAW, ...)`:
    - `L1D_PEND_MISS.PENDING`: `0x00000148` (`event=0x48, umask=0x01`)
    - `CYCLE_ACTIVITY.STALLS_MEM_ANY`: `0x140014a3` (`event=0xa3, umask=0x14, cmask=0x14`)
    - `CYCLE_ACTIVITY.STALLS_TOTAL`: `0x040004a3` (`event=0xa3, umask=0x04, cmask=0x04`)
    - `RESOURCE_STALLS.ANY`: `0x000001a2` (`event=0xa2, umask=0x01`)
  - All four counters were verified openable via `perf_event_open` on this
    `Intel(R) Xeon(R) w5-2565X` (kernel `6.17.0-1033-oem`, `perf_event_paranoid=1`)
    and sanity-checked on `bench_compute` vs `bench_memory` (memory stalls ~15.6x
    higher on `bench_memory`).
  - **Simplification:** On each tick, `PieLikePolicy.on_tick()` computes
    `mem_stall_frac = STALLS_MEM_ANY / cpu_cycles` and `mlp = L1D_PEND_MISS.PENDING / STALLS_MEM_ANY`,
    assigning threads with `mem_stall_frac >= 0.50` and `mlp < 2.0` to `e_cores`
    and all others to `p_cores`.
  - If any of the four counters fails `perf_event_open`, `PieLikePolicy.setup()`
    still **fails loudly** with the exact `errno` and refuses to substitute estimates.

---

## 4. `hass-like` (Reimplementation Inspired by Shelepov et al., 2009)

> **Disclaimer:** This is **NOT** the original HASS implementation. It is a
> simplified reimplementation inspired by Shelepov et al., *"HASS: A Scheduler
> for Heterogeneous Multicore Systems"* (ACM SIGOPS OSR 2009).

- **What the original paper did:**
  - HASS attaches an offline-generated architectural signature (e.g., cache-miss /
    IPC sensitivity profile across core types) to each application binary and uses
    it at dispatch time to select the best core type without online migration
    probing.
  - TODO: verify exact signature vector fields and ELF embedding details against
    Shelepov et al. (2009).
- **What our `hass-like` implementation does and why:**
  - Builds a per-application JSON signature (`results/profiles/hass_<app>_<ts>.json`)
    exclusively from **tuning-split** offline profiling runs (measuring IPC, MPKI,
    and P/E EDP ratio) stored separately from evaluation runs.
  - At evaluation time, `HassLikePolicy` loads the tuning signature (refusing to
    run if no tuning signature exists or if a signature was built from an
    evaluation variant) and pins threads to the pool selected by the signature.

---

## 5. Tuning vs. Evaluation Split (`config/workload_split.yaml`)

All workload variants are partitioned into `tuning` and `evaluation` splits in
`config/workload_split.yaml`:
- **`tuning` variants** (smaller matrix/buffer sizes, smaller GAPBS graph scale
  18 with seed 101, smaller Redis keyspace/requests) are used for parameter
  selection, `oracle-static` profiling, and `hass-like` signature generation.
- **`evaluation` variants** (larger working sets, GAPBS graph scale 20 with seed
  2026, larger Redis keyspace/requests) are reserved strictly for final
  measurement. Both `harness/policies.py` and `harness/workloads/` enforce this
  split and **refuse** to run parameter selection or offline profiling on any
  `evaluation` variant.
