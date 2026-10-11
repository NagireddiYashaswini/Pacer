#include "scheduler_daemon.h"
#include <dirent.h>
#include <signal.h>

static void parse_daemon_config(daemon_config_t *cfg, const char *path) {
    /* Set robust defaults */
    cfg->sampling_interval_ms = 250;
    cfg->mpki_threshold = 15.0;
    cfg->sliding_window_size = 10;
    cfg->phase_variance_threshold = 0.08;
    cfg->phase_mean_shift_threshold = 0.25;
    cfg->cooldown_ms = 1500;
    cfg->hysteresis_ticks = 2;
    cfg->enable_edp_optimization = true;
    cfg->p_core_power_multiplier = 2.5;
    cfg->enable_predictive_scheduler = true;
    cfg->migration_cost_ms = DEFAULT_MIGRATION_COST_MS;
    cfg->min_profitable_dwell_ms = DEFAULT_MIN_PROFITABLE_DWELL_MS;
    strncpy(cfg->p_cores_str, "0-7", sizeof(cfg->p_cores_str));
    strncpy(cfg->e_cores_str, "8-17", sizeof(cfg->e_cores_str));

    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[128];
        if (sscanf(line, " %63[^= ] = %127s", key, val) == 2) {
            if (strcmp(key, "sampling_interval_ms") == 0) cfg->sampling_interval_ms = atoi(val);
            else if (strcmp(key, "mpki_threshold") == 0) cfg->mpki_threshold = atof(val);
            else if (strcmp(key, "sliding_window_size") == 0) cfg->sliding_window_size = atoi(val);
            else if (strcmp(key, "phase_variance_threshold") == 0) cfg->phase_variance_threshold = atof(val);
            else if (strcmp(key, "phase_mean_shift_threshold") == 0) cfg->phase_mean_shift_threshold = atof(val);
            else if (strcmp(key, "cooldown_ms") == 0) cfg->cooldown_ms = atoi(val);
            else if (strcmp(key, "hysteresis_ticks") == 0) cfg->hysteresis_ticks = atoi(val);
            else if (strcmp(key, "enable_predictive_scheduler") == 0) cfg->enable_predictive_scheduler = (strcmp(val, "true") == 0 || atoi(val) == 1);
            else if (strcmp(key, "migration_cost_ms") == 0) cfg->migration_cost_ms = atof(val);
            else if (strcmp(key, "min_profitable_dwell_ms") == 0) cfg->min_profitable_dwell_ms = atof(val);
            else if (strcmp(key, "p_cores") == 0) strncpy(cfg->p_cores_str, val, sizeof(cfg->p_cores_str));
            else if (strcmp(key, "e_cores") == 0) strncpy(cfg->e_cores_str, val, sizeof(cfg->e_cores_str));
        }
    }
    fclose(f);
}

int daemon_init(scheduler_daemon_t *daemon, const char *config_file) {
    if (!daemon) return -1;
    memset(daemon, 0, sizeof(*daemon));

    parse_daemon_config(&daemon->config, config_file);

    /* Initialize hardware topology */
    if (topology_init(&daemon->topo, config_file) != 0) {
        LOG_ERROR("Failed to initialize CPU topology");
        return -1;
    }

    /* Initialize Intel RAPL energy meter */
    rapl_init(&daemon->rapl);

    /* Open telemetry CSV for plotting */
    daemon->telemetry_csv = fopen("results/daemon_telemetry.csv", "w");
    if (daemon->telemetry_csv) {
        fprintf(daemon->telemetry_csv,
                "timestamp_sec,pid,tid,current_cpu,core_pool,ipc,mpki,delta_cycles,delta_inst,phase_shift,migrations\n");
        fflush(daemon->telemetry_csv);
    }

    daemon->total_ticks = 0;
    daemon->total_tick_time_ns = 0;
    daemon->max_tick_time_ns = 0;
    daemon->min_tick_time_ns = UINT64_MAX;

    daemon->running = true;
    LOG_INFO("Hybrid Scheduler Daemon initialized successfully.");
    LOG_INFO("Config: SampleInterval=%d ms, Cooldown=%d ms, Hysteresis=%d ticks, MPKI_Threshold=%.1f, Predictive=%s (Cost=%.1f ms)",
             daemon->config.sampling_interval_ms, daemon->config.cooldown_ms,
             daemon->config.hysteresis_ticks, daemon->config.mpki_threshold,
             daemon->config.enable_predictive_scheduler ? "ENABLED" : "DISABLED",
             daemon->config.migration_cost_ms);
    return 0;
}

int daemon_add_target_pid(scheduler_daemon_t *daemon, pid_t pid) {
    if (!daemon || pid <= 0) return -1;
    for (int i = 0; i < daemon->num_target_pids; i++) {
        if (daemon->target_pids[i] == pid) return 0;
    }
    if (daemon->num_target_pids >= 64) return -1;
    daemon->target_pids[daemon->num_target_pids++] = pid;
    LOG_INFO("Added target PID %d to scheduler monitoring pool.", pid);
    return 0;
}

int daemon_remove_target_pid(scheduler_daemon_t *daemon, pid_t pid) {
    if (!daemon) return -1;
    for (int i = 0; i < daemon->num_target_pids; i++) {
        if (daemon->target_pids[i] == pid) {
            for (int j = i; j < daemon->num_target_pids - 1; j++) {
                daemon->target_pids[j] = daemon->target_pids[j + 1];
            }
            daemon->num_target_pids--;
            LOG_INFO("Removed target PID %d from monitoring pool.", pid);
            return 0;
        }
    }
    return -1;
}

void daemon_discover_threads(scheduler_daemon_t *daemon) {
    if (!daemon) return;

    /* Auto-discover any active workloads communicating via /dev/shm */
    DIR *shm_dir = opendir("/dev/shm");
    if (shm_dir) {
        struct dirent *entry;
        while ((entry = readdir(shm_dir)) != NULL) {
            pid_t p = 0, t = 0;
            if (sscanf(entry->d_name, "hybridsched_%d_%d", &p, &t) == 2) {
                if (p > 0 && kill(p, 0) == 0) {
                    daemon_add_target_pid(daemon, p);
                }
            }
        }
        closedir(shm_dir);
    }

    for (int p = 0; p < daemon->num_target_pids; p++) {
        pid_t pid = daemon->target_pids[p];
        pid_t tids[64];
        int n_tids = pmu_discover_tids(pid, tids, 64);

        for (int i = 0; i < n_tids; i++) {
            pid_t tid = tids[i];
            bool found = false;

            for (int t = 0; t < daemon->num_threads; t++) {
                if (daemon->threads[t].pid == pid && daemon->threads[t].tid == tid) {
                    daemon->threads[t].active = true;
                    found = true;
                    break;
                }
            }

            if (!found && daemon->num_threads < MAX_THREADS) {
                thread_sched_ctx_t *new_t = &daemon->threads[daemon->num_threads++];
                new_t->pid = pid;
                new_t->tid = tid;
                new_t->active = true;
                new_t->candidate_consecutive_ticks = 0;
                new_t->total_migrations = 0;
                new_t->last_migration_ms = (uint64_t)(get_time_ms());

                /* Initialize PMU context */
                pmu_init_thread_ctx(&new_t->pmu_ctx, pid, tid);

                /* Initialize Phase Detector */
                phase_detector_init(&new_t->phase_det,
                                    daemon->config.sliding_window_size,
                                    daemon->config.phase_variance_threshold,
                                    daemon->config.phase_mean_shift_threshold);

                /* Initialize Online Phase Predictor (Stage 3) */
                predictor_init(&new_t->predictor, 0.5, 0.7, daemon->config.migration_cost_ms);
                new_t->predictor.min_profitable_dwell_ms = daemon->config.min_profitable_dwell_ms;

                /* Classify initial core pool */
                int cur_cpu = pmu_get_thread_cpu(pid, tid);
                new_t->current_pool = topology_get_core_type(&daemon->topo, cur_cpu);
                if (new_t->current_pool == CORE_TYPE_UNKNOWN) {
                    new_t->current_pool = CORE_TYPE_P; /* Default initial placement */
                }
                new_t->candidate_pool = new_t->current_pool;

                LOG_INFO("[THREAD ATTACH] Monitored PID %d -> TID %d attached (Initial Core %d: %s)",
                         pid, tid, cur_cpu, core_type_to_str(new_t->current_pool));
            }
        }
    }
}

void daemon_sample_and_schedule_tick(scheduler_daemon_t *daemon) {
    if (!daemon) return;

    uint64_t now_ms = (uint64_t)get_time_ms();
    double now_sec = get_time_sec();

    /* Discover new threads and prune dead threads */
    daemon_discover_threads(daemon);

    for (int i = 0; i < daemon->num_threads; i++) {
        thread_sched_ctx_t *t = &daemon->threads[i];
        if (!t->active) continue;

        pmu_metrics_t metrics;
        if (pmu_sample_thread(&t->pmu_ctx, &metrics) != 0) {
            /* Thread likely terminated */
            pmu_close_thread_ctx(&t->pmu_ctx);
            t->active = false;
            LOG_DEBUG("[THREAD DETACH] TID %d terminated", t->tid);
            continue;
        }

        phase_stat_t pstats;
        bool is_phase_shift = phase_detector_push(&t->phase_det, metrics.ipc, metrics.mpki, &pstats);
        t->last_metrics = metrics;
        t->last_phase_stats = pstats;

        /* Stage 3: Update Online Phase Predictor */
        predictor_update_metrics(&t->predictor, metrics.ipc, metrics.mpki,
                                 (double)daemon->config.sampling_interval_ms, (double)now_ms);

        /* ================= DECISION ENGINE ================= */
        core_type_t desired_pool = t->current_pool;
        char decision_reason[96] = "STABLE";
        bool is_proactive_migration = false;

        /* If Predictive Scheduler is enabled, evaluate proactive policy first */
        if (daemon->config.enable_predictive_scheduler) {
            prediction_decision_t pdec = predictor_evaluate_scheduling(&t->predictor, t->current_pool);
            if (pdec.migration_profitable) {
                desired_pool = pdec.recommended_core_type;
                is_proactive_migration = true;
                snprintf(decision_reason, sizeof(decision_reason),
                         "PRED_PROACTIVE(%s, P=%.2f, Tau=%.0fms)",
                         predictor_phase_to_str(pdec.predicted_next_phase),
                         pdec.confidence, pdec.expected_dwell_ms);
            }
        }

        /* Fallback to reactive decision if no proactive decision was made */
        if (!is_proactive_migration) {
            /* 1. Memory-Stall Penalty Awareness (LLC miss & MPKI filter) */
            if (metrics.mpki >= daemon->config.mpki_threshold) {
                desired_pool = CORE_TYPE_E;
                snprintf(decision_reason, sizeof(decision_reason),
                         "MEM_STALL_GUARD(MPKI=%.1f>%.1f)", metrics.mpki, daemon->config.mpki_threshold);
            }
            /* 2. Compute-bound EDP Optimization */
            else if (metrics.mpki < 5.0 && metrics.ipc >= 1.2) {
                desired_pool = CORE_TYPE_P;
                snprintf(decision_reason, sizeof(decision_reason),
                         "COMPUTE_P_BOOST(IPC=%.2f)", metrics.ipc);
            }
            /* 3. EDP Energy-Saving on Low-IPC non-compute threads */
            else if (metrics.ipc < 0.6 && daemon->config.enable_edp_optimization) {
                desired_pool = CORE_TYPE_E;
                snprintf(decision_reason, sizeof(decision_reason),
                         "EDP_OPTIMAL_E(IPC=%.2f)", metrics.ipc);
            }
        }

        /* Log telemetry for plotting */
        if (daemon->telemetry_csv) {
            fprintf(daemon->telemetry_csv, "%.3f,%d,%d,%d,%s,%.3f,%.2f,%.0f,%.0f,%d,%d\n",
                    now_sec, t->pid, t->tid, metrics.current_cpu,
                    core_type_to_str(t->current_pool), metrics.ipc, metrics.mpki,
                    metrics.delta_cycles, metrics.delta_instructions,
                    is_phase_shift ? 1 : 0, t->total_migrations);
            fflush(daemon->telemetry_csv);
        }

        /* ============= ANTI-THRASHING STATE MACHINE ============= */
        if (desired_pool == t->current_pool) {
            t->candidate_consecutive_ticks = 0;
            t->candidate_pool = t->current_pool;
            continue;
        }

        /* Check cooldown timer (T_cooldown) */
        uint64_t dwell_time_ms = now_ms - t->last_migration_ms;
        uint64_t effective_cooldown = is_proactive_migration ?
                                      (uint64_t)daemon->config.min_profitable_dwell_ms :
                                      (uint64_t)daemon->config.cooldown_ms;

        if (dwell_time_ms < effective_cooldown) {
            LOG_DEBUG("[COOLDOWN] TID %d migration to %s suppressed (dwell=%lu ms < cooldown=%lu ms)",
                      t->tid, core_type_to_str(desired_pool),
                      (unsigned long)dwell_time_ms, (unsigned long)effective_cooldown);
            continue;
        }

        /* For reactive fallback, require hysteresis tick confirmation */
        if (!is_proactive_migration) {
            if (desired_pool != t->candidate_pool) {
                t->candidate_pool = desired_pool;
                t->candidate_consecutive_ticks = 1;
            } else {
                t->candidate_consecutive_ticks++;
            }

            if (t->candidate_consecutive_ticks < daemon->config.hysteresis_ticks) {
                LOG_DEBUG("[HYSTERESIS] TID %d candidate %s held (tick %d/%d)",
                          t->tid, core_type_to_str(desired_pool),
                          t->candidate_consecutive_ticks, daemon->config.hysteresis_ticks);
                continue;
            }
        }

        /* ============= EXECUTE MIGRATION ============= */
        cpu_set_t *target_mask = (desired_pool == CORE_TYPE_P) ?
                                 &daemon->topo.p_core_mask : &daemon->topo.e_core_mask;

        int ret = sched_setaffinity(t->tid, sizeof(cpu_set_t), target_mask);
        if (ret == 0) {
            int prev_cpu = metrics.current_cpu;
            core_type_t prev_pool = t->current_pool;
            t->current_pool = desired_pool;
            t->last_migration_ms = now_ms;
            t->total_migrations++;
            t->candidate_consecutive_ticks = 0;

            LOG_INFO(COLOR_MAGENTA "[MIGRATION #%d] TID %d moved: %s (CPU %d) -> %s | Reason: %s (IPC=%.2f, MPKI=%.1f)" COLOR_RESET,
                     t->total_migrations, t->tid,
                     core_type_to_str(prev_pool), prev_cpu,
                     core_type_to_str(desired_pool), decision_reason,
                     metrics.ipc, metrics.mpki);

            if (daemon->total_migrations_logged < MAX_LOGGED_MIGRATIONS) {
                migration_event_t *ev = &daemon->migration_log[daemon->total_migrations_logged++];
                ev->pid = t->pid;
                ev->tid = t->tid;
                ev->timestamp_sec = now_sec;
                ev->from_cpu = prev_cpu;
                ev->from_type = prev_pool;
                ev->to_type = desired_pool;
                ev->ipc = metrics.ipc;
                ev->mpki = metrics.mpki;
                snprintf(ev->reason, sizeof(ev->reason), "%s", decision_reason);
            }
        } else {
            LOG_WARN("sched_setaffinity failed for TID %d (errno=%d: %s)",
                     t->tid, errno, strerror(errno));
        }
    }
}

void daemon_run_loop(scheduler_daemon_t *daemon) {
    if (!daemon) return;
    LOG_INFO("Adaptive Scheduling Daemon loop running (tick = %d ms)...",
             daemon->config.sampling_interval_ms);

    useconds_t sleep_us = daemon->config.sampling_interval_ms * 1000;
    while (daemon->running) {
        uint64_t t_start = get_time_ns();
        daemon_sample_and_schedule_tick(daemon);
        uint64_t t_end = get_time_ns();
        uint64_t dt_ns = (t_end >= t_start) ? (t_end - t_start) : 0;

        daemon->total_ticks++;
        daemon->total_tick_time_ns += dt_ns;
        if (dt_ns > daemon->max_tick_time_ns) daemon->max_tick_time_ns = dt_ns;
        if (dt_ns < daemon->min_tick_time_ns) daemon->min_tick_time_ns = dt_ns;

        usleep(sleep_us);
    }
}

void daemon_stop(scheduler_daemon_t *daemon) {
    if (daemon) daemon->running = false;
}

void daemon_cleanup(scheduler_daemon_t *daemon) {
    if (!daemon) return;
    for (int i = 0; i < daemon->num_threads; i++) {
        if (daemon->threads[i].active) {
            pmu_close_thread_ctx(&daemon->threads[i].pmu_ctx);
        }
    }
    rapl_close(&daemon->rapl);
    if (daemon->telemetry_csv) {
        fclose(daemon->telemetry_csv);
        daemon->telemetry_csv = NULL;
    }
    double avg_us = (daemon->total_ticks > 0) ? ((double)daemon->total_tick_time_ns / (double)daemon->total_ticks / 1000.0) : 0.0;
    double max_us = (double)daemon->max_tick_time_ns / 1000.0;
    double min_us = (daemon->min_tick_time_ns != UINT64_MAX) ? ((double)daemon->min_tick_time_ns / 1000.0) : 0.0;
    LOG_INFO("Daemon profiling: Ticks=%" PRIu64 ", Avg Tick=%.2f us, Max Tick=%.2f us, Min Tick=%.2f us",
             daemon->total_ticks, avg_us, max_us, min_us);

    if (daemon->config.enable_predictive_scheduler) {
        uint64_t total_preds = 0, total_recs = 0, total_vetos_cost = 0, total_vetos_unc = 0;
        for (int i = 0; i < daemon->num_threads; i++) {
            total_preds += daemon->threads[i].predictor.total_predictions;
            total_recs += daemon->threads[i].predictor.migrations_recommended;
            total_vetos_cost += daemon->threads[i].predictor.migrations_suppressed_cost;
            total_vetos_unc += daemon->threads[i].predictor.migrations_suppressed_uncertainty;
        }
        printf(COLOR_GREEN "[PREDICTOR TELEMETRY] Total Predictions=%" PRIu64 ", Proactive Approved=%" PRIu64 ", Vetoes (Cost)=%" PRIu64 ", Vetoes (Uncertainty)=%" PRIu64 "\n" COLOR_RESET,
               total_preds, total_recs, total_vetos_cost, total_vetos_unc);
    }

    LOG_INFO("Hybrid Scheduler Daemon shut down cleanly (Total Migrations: %d).",
             daemon->total_migrations_logged);
}
