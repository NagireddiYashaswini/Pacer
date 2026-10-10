#ifndef SCHEDULER_DAEMON_H
#define SCHEDULER_DAEMON_H

#include "common.h"
#include "topology.h"
#include "pmu_sampler.h"
#include "rapl_energy.h"
#include "phase_detector.h"

#define MAX_LOGGED_MIGRATIONS 2048

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
} daemon_config_t;

typedef struct {
    cpu_topology_t topo;
    rapl_meter_t rapl;
    daemon_config_t config;

    thread_sched_ctx_t threads[MAX_THREADS];
    int num_threads;

    pid_t target_pids[64];
    int num_target_pids;

    migration_event_t migration_log[MAX_LOGGED_MIGRATIONS];
    int total_migrations_logged;

    volatile bool running;
    FILE *log_file;
    FILE *telemetry_csv;
} scheduler_daemon_t;

int daemon_init(scheduler_daemon_t *daemon, const char *config_file);
int daemon_add_target_pid(scheduler_daemon_t *daemon, pid_t pid);
int daemon_remove_target_pid(scheduler_daemon_t *daemon, pid_t pid);
void daemon_discover_threads(scheduler_daemon_t *daemon);
void daemon_sample_and_schedule_tick(scheduler_daemon_t *daemon);
void daemon_run_loop(scheduler_daemon_t *daemon);
void daemon_stop(scheduler_daemon_t *daemon);
void daemon_cleanup(scheduler_daemon_t *daemon);

#endif /* SCHEDULER_DAEMON_H */
