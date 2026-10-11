# Related Work & Novelty Assessment: `hybridsched`

**Auditor Role**: Senior Computer Systems Researcher & Peer Reviewer (ASPLOS/EuroSys/TPDS)  
**Date**: 2026-10-11  

---

## 1. Prior Art Taxonomy & Comparative Analysis

Thread scheduling across heterogeneous cores and dynamic microarchitectural phase tracking have been extensively studied across operating systems and computer architecture literature over the past two decades.

```
                                ┌──────────────────────────────────────────────┐
                                │ CPU Scheduling & Phase Management Literature │
                                └──────────────────────┬───────────────────────┘
                                                       │
         ┌──────────────────────────────┬──────────────┴───────────────┬──────────────────────────────┐
         ▼                              ▼                              ▼                              ▼
┌──────────────────┐          ┌───────────────────┐          ┌───────────────────┐          ┌───────────────────┐
│ Kernel / HW Tech │          │ Phase Detection   │          │ Heterogeneous     │          │ Contention-Aware  │
│ - Linux CFS/EEVDF│          │ - SimPoint (BBV)  │          │   Schedulers      │          │   Placement       │
│ - Linux EAS      │          │ - Dynamic PMU     │          │ - PIE / AMP-Sched │          │ - Bubble-Up       │
│ - Intel ITD/HFI  │          │ - Markov Predict  │          │ - Hysteresis Migr │          │ - Paragon/Heracles│
└──────────────────┘          └───────────────────┘          └───────────────────┘          └───────────────────┘
```

### 1.1 Linux Kernel Schedulers & Hardware Support
- **Completely Fair Scheduler (CFS) & EEVDF**: Standard Linux desktop/server schedulers optimize for fairness and virtual runtime (`vruntime`). CFS is agnostic to microarchitectural phase dynamics (e.g., whether a thread is arithmetic-bound or memory-stalled) and relies on load balancing heuristics (`select_task_rq_fair`).
- **Linux Energy-Aware Scheduling (EAS)**: Deployed on ARM big.LITTLE mobile platforms. EAS relies on an *Energy Model* (EM) populated with static power-performance curves per OPP (Operating Performance Point). However, EAS uses capacity and utilization estimation (`PELT`), lacking online microarchitectural phase prediction.
- **Intel Thread Director (ITD / HFI)**: Introduced in Alder Lake (12th Gen). ITD provides hardware-level telemetry to the OS via Enhanced Hardware Feedback Interface (EHFI), classifying instructions into classes (Class 0: normal, Class 1: AVX2, Class 2: AVX-512/AMX, Class 3: Deep Neural Net/Stall). While reactive ITD guidance is implemented in kernel 5.18+, it lacks forward-looking transition prediction and operates on fixed hardware-defined classification heuristics.

### 1.2 Academic Phase Detection & Prediction
- **Sherwood et al. (ASPLOS 2002, SimPoint)**: Pioneered Basic Block Vectors (BBV) for offline phase classification. While highly accurate, BBVs incur prohibitive runtime overhead for online operating system scheduling.
- **Dhodapkar & Smith (MICRO 2002)**: Compared instruction working sets, basic block vectors, and branch predictability for dynamic phase detection, proving that sliding-window hardware metrics can capture phase shifts.
- **Koukos et al. (CGO 2013)** and **Becchi et al. (CF 2010)**: Explored online dynamic PMU sampling for heterogeneous core assignment, primarily using reactive thresholds on IPC and memory intensity.
- **Markov-based Architectural Prediction**: Markov chains have been applied to branch prediction (Chen et al.), prefetching (Joseph & Grunwald), and power management (Simunic et al., DATE 2001). Applying online Markov models with Laplace smoothing to operating system thread migration is a logical extension.

### 1.3 Asymmetric Multicore Scheduling & Migration Cost Models
- **Van Craeynest et al. (ISCA 2012, PIE)**: Proposed Performance Impact Estimation (PIE) to estimate IPC speedup on big cores versus small cores using runtime CPI stacks.
- **Kumar et al. (MICRO 2003, ASPLOS 2004)**: Foundational work on heterogeneous multi-core architectures showing that phase-directed core swapping achieves substantial energy reductions.
- **Migration Cost & Cache Warm-Up**: Torrellas et al. (TOCS 1995) and Vasudevan et al. (PACT 2013) established that migrating a cache-heavy thread destroys warm cache state, requiring millions of cycles to refill cache lines. Effective schedulers require residency cooldowns ($T_{\text{cooldown}}$) or break-even dwell thresholds ($\tau_{\min}$).

---

## 2. Positioning of `hybridsched`

| Dimension | Existing State of the Art | `hybridsched` Design | Novelty Status |
| :--- | :--- | :--- | :--- |
| **Phase Detection** | Sliding-window variance & mean testing on hardware PMUs | Sliding-window IPC variance $\sigma^2$ and MPKI barrier | **Standard** (well-known statistical changepoint technique). |
| **Phase Transition Prediction** | Offline profiling or reactive threshold state machines | Online 2-state Markov chain with Laplace smoothing & dwell EMA | **Incremental Synthesis** (application of known model to userspace scheduler). |
| **Migration Decision Engine** | Fixed hysteresis counts and static residency timers | Analytic break-even threshold $\tau_{\min}$ + uncertainty penalty $\gamma$ | **Modest Novelty** (principled veto of uncertain phase shifts). |
| **Implementation Layer** | In-kernel patches (EAS, sched_ext) | Non-invasive user-space daemon using `/dev/shm` / `sched_setaffinity` | **Engineering Artifact** (practical but not conceptually novel). |
| **Hardware Platform** | Native hybrid silicon (Alder Lake / Raptor Lake) | Homogeneous Intel Xeon with software affinity partitions | **Major Limitation** (restricts claims). |

---

## 3. Honest Novelty & Contribution Assessment

### What CANNOT Be Claimed:
1. **NO novelty in physical hybrid CPU energy optimization**: Because the experimental host has zero E-cores and frequency clamping was never applied, claiming physical energy reductions on hybrid x86 CPUs is invalid.
2. **NO novelty in inventing Markov phase prediction**: Markov modeling of workload phases has been documented since the late 1990s in computer architecture.
3. **NO claim that user-space scheduling outperforms kernel-native scheduling**: A user-space daemon polling at 100 ms has inherent sampling delay and context-switch overhead that an in-kernel scheduler or BPF-based `sched_ext` controller avoids.

### What CAN Be Defensibly Claimed:
1. **A Lightweight Online Phase-Transition Predictor for Thread Steering**:
   A concrete, $O(1)$ implementation combining Laplace-smoothed transition probabilities and exponential moving average (EMA) dwell times that operates in $< 15\,\mu s$ per scheduling tick.
2. **An Empirical Characterization of the Prediction Break-Even Frontier**:
   A rigorous study analyzing at what phase granularities ($\tau$) predictive migration reduces migration lag relative to reactive hysteresis, and identifying the break-even point where migration overhead exceeds phase duration benefits.
3. **A Negative Result / Architectural Reality Check**:
   Transparent demonstration that without hardware PMU access (`perf_event_paranoid <= 1`) and physical core heterogeneity, user-space hybrid schedulers cannot realize their theoretical EDP benefits, warning future researchers against synthetic fallback evaluation traps.
