#ifndef PMU_SAMPLER_H
#define PMU_SAMPLER_H

#include "common.h"
#include <linux/perf_event.h>
#include <sys/syscall.h>

typedef struct {
    uint64_t cycles;
    uint64_t instructions;
    uint64_t llc_misses;
    uint64_t timestamp_ns;
} pmu_raw_sample_t;

typedef struct {
    double delta_cycles;
    double delta_instructions;
    double delta_llc_misses;
    double delta_time_sec;
    double ipc;
    double mpki;          /* Misses Per Kilo-Instructions: (LLC_misses * 1000) / instructions */
    int current_cpu;
} pmu_metrics_t;

typedef struct {
    pid_t pid;
    pid_t tid;
    int leader_fd;        /* Cycles (group leader) */
    int inst_fd;          /* Instructions */
    int llc_fd;           /* LLC Cache Misses */
    bool pmu_active;
    bool fallback_active;

    pmu_raw_sample_t prev_sample;
    pmu_metrics_t last_metrics;

    /* Fallback proc stat counters */
    uint64_t prev_utime_ticks;
    uint64_t prev_stime_ticks;
    int prev_cpu;
} thread_pmu_ctx_t;

/* Discovery of threads */
int pmu_discover_tids(pid_t pid, pid_t *tids_out, int max_tids);

/* Sampler initialization & cleanup */
int pmu_init_thread_ctx(thread_pmu_ctx_t *ctx, pid_t pid, pid_t tid);
void pmu_close_thread_ctx(thread_pmu_ctx_t *ctx);

/* Sample metrics atomically using PERF_FORMAT_GROUP */
int pmu_sample_thread(thread_pmu_ctx_t *ctx, pmu_metrics_t *metrics);

/* Helper to get current CPU of a TID */
int pmu_get_thread_cpu(pid_t pid, pid_t tid);

#endif /* PMU_SAMPLER_H */
