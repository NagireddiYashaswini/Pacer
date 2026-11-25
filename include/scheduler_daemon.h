#ifndef SCHEDULER_DAEMON_H
#define SCHEDULER_DAEMON_H

#include "common.h"
#include "topology.h"
#include "pmu_sampler.h"
#include "rapl_energy.h"
#include "phase_detector.h"

#define MAX_LOGGED_MIGRATIONS 2048
#define MAX_PHASE_CACHE 16

typedef enum {
    SCHED_POLICY_SPEEDUP = 0,
    SCHED_POLICY_MPKI    = 1
} sched_policy_t;

typedef enum {
    PROBE_IDLE = 0,
    PROBE_WARMUP,         /* discarding discard_ticks after migration to dest */
    PROBE_MEASURING,      /* measuring instr/sec over probe_window_ticks */
    PROBE_RETURN_WARMUP   /* discarding discard_ticks after migrating back */
} probe_phase_t;

typedef struct {
    uint64_t phase_id;
    double   speedup;          /* throughput_on_P / throughput_on_E */
    uint64_t last_updated_ms;
    int      sample_count;
    bool     valid;
} speedup_cache_entry_t;

/*
 * Rule 3: provenance snapshot captured at daemon startup and stamped on
 * every telemetry/decision/probe CSV row.
 */
typedef struct {
    char git_commit[64];          /* HEAD SHA-1 (short) */
    char kernel_version[128];     /* uname -r */
    char cpu_model[256];          /* /proc/cpuinfo model name */
    char governor[64];            /* cpufreq governor (first CPU) */
    char turbo_state[16];         /* "enabled" | "disabled" | "unknown" */
    char rapl_source[64];         /* "intel-rapl:0" or "unavailable" */
    char p_cores_pool[128];       /* P-core CPU list */
    char e_cores_pool[128];       /* E-core CPU list */
    char excluded_siblings[128];  /* Excluded SMT siblings */
    int  trial_index;             /* set by caller before run */
} run_provenance_t;

typedef struct {
    pid_t pid;
    pid_t tid;
    double timestamp_sec;
    int from_cpu;
    core_type_t from_type;
    core_type_t to_type;
    double ipc;
    double mpki;
    char reason[64];
} migration_event_t;

typedef struct {
    pid_t pid;
    pid_t tid;
    thread_pmu_ctx_t pmu_ctx;
    phase_detector_t phase_det;

    core_type_t current_pool;     /* Pool thread is currently pinned to */
    core_type_t candidate_pool;   /* Desired pool from telemetry evaluation */
    int candidate_consecutive_ticks;

    uint64_t last_migration_ms;
    int total_migrations;
    bool active;

    pmu_metrics_t last_metrics;
    phase_stat_t last_phase_stats;

    /* Per-thread baseline throughput EMA on current pool (instr/sec) */
    double current_ips_ema;
    int    current_ips_samples;

    /* Per-(thread, phase_id) speedup cache */
    speedup_cache_entry_t speedup_cache[MAX_PHASE_CACHE];

    /* Probe protocol state */
    probe_phase_t probe_state;
    uint64_t      probe_phase_id;
    core_type_t   probe_source_pool;
    core_type_t   probe_dest_pool;
    int           probe_target_cpu;
    int           probe_ticks_remaining;
    double        probe_before_ips;
    double        probe_accum_inst;
    double        probe_accum_sec;
    double        probe_last_mpki;
    uint64_t      last_probe_ms;
    uint64_t      last_skip_log_ms;
    int           total_probes;
} thread_sched_ctx_t;

typedef struct {
    int sampling_interval_ms;
    double mpki_threshold;
    int sliding_window_size;
    double phase_variance_threshold;
    double phase_mean_shift_threshold;
    int cooldown_ms;
    int hysteresis_ticks;
    bool enable_edp_optimization;
    double p_core_power_multiplier;
    char p_cores_str[128];
    char e_cores_str[128];

    /* Speedup estimator & probe protocol parameters */
    sched_policy_t policy;
    int    probe_window_ticks;
    int    discard_ticks;
    double probe_rate_cap;    /* max probes per second per thread */
    double decay_alpha;       /* exponential decay weight for new samples */
    uint64_t estimate_ttl_ms; /* cache freshness window */
} daemon_config_t;

typedef struct {
    cpu_topology_t topo;
    rapl_meter_t rapl;
    daemon_config_t config;
    run_provenance_t provenance;  /* Rule 3: captured at init */
    pmu_hybrid_info_t hybrid;     /* Detected once at startup */

    thread_sched_ctx_t threads[MAX_THREADS];
    int num_threads;

    pid_t target_pids[64];
    int num_target_pids;

    migration_event_t migration_log[MAX_LOGGED_MIGRATIONS];
    int total_migrations_logged;

    /* Per-CPU utilization snapshot from /proc/stat for idle-core probing */
    uint64_t prev_cpu_busy[MAX_CPUS];
    uint64_t prev_cpu_total[MAX_CPUS];
    double   cpu_busy_pct[MAX_CPUS];
    bool     has_cpu_stat_prev;

    volatile bool running;
    FILE *log_file;
    FILE *telemetry_csv;
    FILE *probe_csv;
} scheduler_daemon_t;

int daemon_init(scheduler_daemon_t *daemon, const char *config_file);
int daemon_add_target_pid(scheduler_daemon_t *daemon, pid_t pid);
int daemon_remove_target_pid(scheduler_daemon_t *daemon, pid_t pid);
void daemon_discover_threads(scheduler_daemon_t *daemon);
void daemon_sample_and_schedule_tick(scheduler_daemon_t *daemon);
void daemon_run_loop(scheduler_daemon_t *daemon);
void daemon_stop(scheduler_daemon_t *daemon);
void daemon_cleanup(scheduler_daemon_t *daemon);

/* Populate run_provenance_t by reading /proc, sysfs, and git */
int daemon_collect_provenance(run_provenance_t *prov);

/* Shared speedup calculation: throughput_on_P / throughput_on_E */
double scheduler_compute_speedup(double ips_p, double ips_e);

#endif /* SCHEDULER_DAEMON_H */
