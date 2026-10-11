# Hardware & Topology Validation Report: `hybridsched`

**Auditor Role**: Senior Computer Systems Researcher & Performance Specialist  
**Audit Date**: 2026-10-11  
**Target Repository**: `https://github.com/NagireddiYashaswini/Pacer`  

---

## 1. Verified Processor Identity & Microarchitecture

Using `lscpu`, `/proc/cpuinfo`, and Linux kernel sysfs interfaces, the host hardware was independently queried:

| Parameter | Sysfs / Kernel Ground Truth | Documentation Claim | Verification Status |
| :--- | :--- | :--- | :--- |
| **Processor Model** | Intel(R) Xeon(R) w5-2565X | Intel Xeon w5-2565X | **CONFIRMED** |
| **Microarchitecture** | Sapphire Rapids-WS (Intel 7) | Sapphire Rapids | **CONFIRMED** |
| **Family / Model / Stepping** | Family 6, Model 143, Stepping 8 | Family 6, Model 143 | **CONFIRMED** |
| **Physical Sockets** | 1 | 1 | **CONFIRMED** |
| **Physical Cores** | 18 | 18 | **CONFIRMED** |
| **Hardware Threads** | 36 (Hyper-Threading enabled: 2 threads/core) | 36 threads | **CONFIRMED** |
| **L1 Data Cache** | 18 × 48 KiB (864 KiB total) | 48 KiB/core | **CONFIRMED** |
| **L2 Cache** | 18 × 2 MiB (36 MiB total) | 2 MiB/core | **CONFIRMED** |
| **L3 Cache (Shared LLC)**| 37.5 MiB unified | 37.5 MiB / 45 MB | **DISCREPANCY (37.5 MiB real)** |
| **NUMA Nodes** | 1 (Node 0: CPUs 0–35) | 1 | **CONFIRMED** |

---

## 2. Core Heterogeneity Audit: Physical vs. Emulated

### 2.1 Sysfs Hybrid Interface Check
On authentic Intel Hybrid processors (e.g., Alder Lake Core i7-12700K, Raptor Lake Core i7-13700K, Core i7-13620H), the Linux kernel exposes heterogeneous PMU directories:
- `/sys/devices/cpu_core` (Golden Cove / Raptor Cove P-cores)
- `/sys/devices/cpu_atom` (Gracemont / Crestmont E-cores)

**Inspection Result**:
```bash
$ ls -d /sys/devices/cpu_* 2>/dev/null
No /sys/devices/cpu_* found
```
Neither `/sys/devices/cpu_core` nor `/sys/devices/cpu_atom` exists on this host.

### 2.2 Physical Core Microarchitecture
All 18 physical cores on the Intel Xeon w5-2565X are **identical Golden Cove high-performance workstation cores**. There are **zero (0)** physical Gracemont or Crestmont Efficient cores (E-cores) on the die.

### 2.3 CPU Frequency Interfaces & Frequency Clamping Audit
The scheduler documentation and configuration file (`config/daemon.conf`) claimed:
```ini
[emulation]
enable_frequency_clamping = false
e_core_max_freq_khz = 2000000
p_core_max_freq_khz = 4600000
```

We investigated whether frequency clamping was ever applied to emulate asymmetric core performance:
1. **Sysfs scaling frequencies**:
   - `/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq` = `4600000` (4.6 GHz)
   - `/sys/devices/system/cpu/cpu8/cpufreq/scaling_max_freq` = `4600000` (4.6 GHz)
   Both "P-core" (CPU 0) and "E-core" (CPU 8) share the exact same frequency ceiling.
2. **Codebase Implementation**:
   A search across the entire C codebase revealed that `enable_frequency_clamping` and `e_core_max_freq_khz` are **never parsed or referenced** in any source file. The C daemon contains zero logic to invoke `cpufreq` or modify CPU frequencies.
3. **Execution Script (`scripts/setup_env.sh`)**:
   `scripts/setup_env.sh` contains a bash helper to clamp frequencies, but it explicitly requires `root`/`sudo` privileges:
   ```bash
   check_root() {
       if [ "$(id -u)" -ne 0 ]; then
           echo "[!] Sudo/root privileges required..."
           exit 1
       fi
   }
   ```
   Executing `bash scripts/setup_env.sh status` confirms:
   ```text
   Kernel perf_event_paranoid: 4
   Intel RAPL energy_uj: Not readable without root
   --- Sample P-Core (CPU 0) --- Max Freq: 4600000 kHz
   --- Sample E-Core (CPU 8) --- Max Freq: 4600000 kHz
   ```
   No frequency clamping was ever applied during any experiment.

---

## 3. Analysis of Core Classification in the Scheduler

In `src/topology.c`:
```c
int topology_detect_sysfs_hybrid(cpu_topology_t *topo) {
    int has_p = read_sysfs_string("/sys/devices/cpu_core/cpus", p_buf, sizeof(p_buf));
    int has_e = read_sysfs_string("/sys/devices/cpu_atom/cpus", e_buf, sizeof(e_buf));
    if (has_p == 0 && has_e == 0) { ... return 0; }
    return -1;
}
```
Because the host is homogeneous, `topology_detect_sysfs_hybrid()` returns `-1`. The daemon then parses `config/daemon.conf`:
- `p_cores = 0,1,2,3,4,5,6,7`
- `e_cores = 8,9,10,11,12,13,14,15,16,17`

### Verdict:
The CPU pools labelled "P-Core Pool" and "E-Core Pool" are **purely arbitrary software affinity partitions** of identical physical cores residing on the same physical die and shared L3 cache ring. 

When `hybridsched` migrates a thread from CPU 4 to CPU 14:
- It does **not** move the thread to a smaller, more energy-efficient core.
- It does **not** move the thread to a lower-frequency core.
- It does **not** alter the dynamic power consumption of the core.
- It merely incurs a thread migration syscall overhead and invalidates private L1/L2 caches.

---

## 4. Scientific Impact & Invalidation of Claims

| Paper / Project Claim | Scientific Reality | Impact on Research Claim |
| :--- | :--- | :--- |
| "Evaluation demonstrates dynamic thread migration between physical P-cores and E-cores" | The platform has 18 identical P-cores. Zero E-cores exist. | **INVALIDATED**. Cannot claim evaluation on a physical hybrid CPU. |
| "E-cores emulated via frequency clamping to 2.0 GHz" | Frequency clamping was disabled, never implemented in C, and unprivileged user cannot modify cpufreq. Both pools ran at 4.6 GHz. | **INVALIDATED**. Cannot claim emulated frequency asymmetry. |
| "Predictive scheduler achieves 33% EDP reduction vs CFS by placing memory threads on E-cores" | Migrating between identical cores at identical clock speeds does not save energy. Energy numbers were generated by synthetic power lookup tables. | **INVALIDATED**. All reported energy/EDP values are non-empirical artifacts. |
| "Online phase prediction detects memory vs compute phases" | Algorithms (Markov chain, IPC ring buffer, dwell EMA) are genuinely implemented in C, but were fed hardcoded synthetic metrics via `/dev/shm` because `perf_event_open` failed. | **QUALIFIED**. Algorithmic design is real, but online inputs were synthetic. |

---

## 5. Scope of Defensible Research Claims

To remain scientifically rigorous and admissible to top-tier venues (EuroSys/ASPLOS/TPDS):
1. **Remove all claims of physical P-core / E-core evaluation** on this testbed.
2. **Characterize the work truthfully**:
   - The primary contribution is a **predictive online phase-transition framework** with a **migration-cost awareness model** designed for heterogeneous and partitioned environments.
   - On the current testbed, empirical evaluation is strictly limited to **scheduling decision latency, migration overhead, and software phase-tracking dynamics**.
   - True energy-delay product (EDP) benefits on heterogeneous CPUs remain an **unvalidated architectural hypothesis** until executed on authentic hybrid silicon (e.g., Intel Alder Lake / Raptor Lake or ARM big.LITTLE) with accessible hardware PMUs and energy counters.
