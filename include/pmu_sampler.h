#ifndef PMU_SAMPLER_H
#define PMU_SAMPLER_H

#include "common.h"
#include <linux/perf_event.h>
#include <sys/syscall.h>

/* -------------------------------------------------------------------------
 * Hybrid PMU info (detected once at startup via pmu_detect_hybrid()).
 * On homogeneous CPUs both type fields equal PERF_TYPE_HARDWARE.
 * ------------------------------------------------------------------------- */
typedef struct {
    bool     is_hybrid;
    uint32_t type_pcore;   /* perf PMU type for P-cores (cpu_core device) */
    uint32_t type_ecore;   /* perf PMU type for E-cores (cpu_atom device) */
} pmu_hybrid_info_t;

/* Detect and populate hybrid info.  Call once at daemon startup. */
void pmu_detect_hybrid(pmu_hybrid_info_t *info);

/* -------------------------------------------------------------------------
 * Per-sample snapshot stored in the context for delta computation.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t cycles;
    uint64_t instructions;
    uint64_t llc_misses;
    uint64_t timestamp_ns;
} pmu_raw_sample_t;

/* -------------------------------------------------------------------------
 * Derived metrics computed each sampling tick.
 * ------------------------------------------------------------------------- */
typedef struct {
    double delta_cycles;
    double delta_instructions;
    double delta_llc_misses;
    double delta_time_sec;
    double ipc;            /* Instructions Per Cycle */
    double mpki;           /* LLC Misses Per Kilo-Instructions */
    int    current_cpu;
} pmu_metrics_t;

/* -------------------------------------------------------------------------
 * Per-thread PMU context.
 * ------------------------------------------------------------------------- */
typedef struct {
    pid_t pid;
    pid_t tid;
    int   leader_fd;   /* group leader fd: CPU cycles */
    int   inst_fd;     /* sibling fd: instructions */
    int   llc_fd;      /* sibling fd: LLC misses */
    bool  pmu_active;
    uint32_t pmu_type; /* PMU type used (PERF_TYPE_HARDWARE or hybrid type) */

    pmu_raw_sample_t prev_sample;
    pmu_metrics_t    last_metrics;
    int              prev_cpu;
} thread_pmu_ctx_t;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Enumerate threads of a process from /proc/<pid>/task/ */
int pmu_discover_tids(pid_t pid, pid_t *tids_out, int max_tids);

/* Return current CPU for a thread from /proc/<pid>/task/<tid>/stat */
int pmu_get_thread_cpu(pid_t pid, pid_t tid);

/*
 * Initialise a per-thread PMU group context.
 * pmu_init_thread_ctx()    — uses PERF_TYPE_HARDWARE (homogeneous).
 * pmu_init_thread_ctx_ex() — uses hybrid info to pick the correct PMU type.
 *
 * Returns 0 on success, -1 on hard failure (perf_event_open denied, etc.).
 * On -1 a detailed diagnostic has already been printed to stderr.
 */
int pmu_init_thread_ctx(thread_pmu_ctx_t *ctx, pid_t pid, pid_t tid);
int pmu_init_thread_ctx_ex(thread_pmu_ctx_t *ctx, pid_t pid, pid_t tid,
                            const pmu_hybrid_info_t *hybrid);

/* Close all fds in a context */
void pmu_close_thread_ctx(thread_pmu_ctx_t *ctx);

/*
 * Sample the counter group and compute delta metrics.
 * Returns 0 on success, -1 if the read fails (thread exited, or on hybrid
 * CPUs the thread migrated to the other core type — reinitialise the ctx).
 */
int pmu_sample_thread(thread_pmu_ctx_t *ctx, pmu_metrics_t *metrics);

#endif /* PMU_SAMPLER_H */
