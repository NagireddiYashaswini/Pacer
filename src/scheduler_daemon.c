#include "scheduler_daemon.h"
#include <dirent.h>
#include <signal.h>
#include <sys/utsname.h>

/* -------------------------------------------------------------------------
 * Rule 3: collect run-level provenance from /proc, sysfs, and git.
 * All fields are read from real system state — no hardcoded defaults.
 * Returns 0 on success; individual fields that cannot be read are set to
 * "unknown" so the caller knows they are absent, not fabricated.
 * ------------------------------------------------------------------------- */
int daemon_collect_provenance(run_provenance_t *prov) {
    if (!prov) return -1;
    memset(prov, 0, sizeof(*prov));

    /* git commit: run `git rev-parse --short HEAD` */
    {
        FILE *fp = popen("git rev-parse --short HEAD 2>/dev/null", "r");
        if (fp) {
            if (!fgets(prov->git_commit, sizeof(prov->git_commit), fp))
                strncpy(prov->git_commit, "unknown", sizeof(prov->git_commit));
            else {
                /* strip trailing newline */
                size_t l = strlen(prov->git_commit);
                if (l > 0 && prov->git_commit[l-1] == '\n') prov->git_commit[l-1] = '\0';
            }
            pclose(fp);
        } else {
            strncpy(prov->git_commit, "unknown", sizeof(prov->git_commit));
        }
    }

    /* kernel version: uname */
    {
        struct utsname u;
        if (uname(&u) == 0)
            strncpy(prov->kernel_version, u.release, sizeof(prov->kernel_version) - 1);
        else
            strncpy(prov->kernel_version, "unknown", sizeof(prov->kernel_version));
    }

    /* CPU model: first "model name" from /proc/cpuinfo */
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        bool found = false;
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "model name", 10) == 0) {
                    char *colon = strchr(line, ':');
                    if (colon) {
                        char *v = colon + 1;
                        while (*v == ' ' || *v == '\t') v++;
                        strncpy(prov->cpu_model, v, sizeof(prov->cpu_model) - 1);
                        size_t l = strlen(prov->cpu_model);
                        if (l > 0 && prov->cpu_model[l-1] == '\n') prov->cpu_model[l-1] = '\0';
                        found = true;
                        break;
                    }
                }
            }
            fclose(f);
        }
        if (!found) strncpy(prov->cpu_model, "unknown", sizeof(prov->cpu_model));
    }

    /* cpufreq governor: read cpu0 (representative) */
    {
        FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
        if (f) {
            if (!fgets(prov->governor, sizeof(prov->governor), f))
                strncpy(prov->governor, "unknown", sizeof(prov->governor));
            else {
                size_t l = strlen(prov->governor);
                if (l > 0 && prov->governor[l-1] == '\n') prov->governor[l-1] = '\0';
            }
            fclose(f);
        } else {
            strncpy(prov->governor, "unknown", sizeof(prov->governor));
        }
    }

    /* turbo state: Intel no_turbo sysfs node */
    {
        FILE *f = fopen("/sys/devices/system/cpu/intel_pstate/no_turbo", "r");
        if (f) {
            char buf[8] = {0};
            if (fgets(buf, sizeof(buf), f)) {
                strncpy(prov->turbo_state,
                        (buf[0] == '0') ? "enabled" : "disabled",
                        sizeof(prov->turbo_state));
            } else {
                strncpy(prov->turbo_state, "unknown", sizeof(prov->turbo_state));
            }
            fclose(f);
        } else {
            /* Try cpufreq boost (non-pstate kernels) */
            FILE *f2 = fopen("/sys/devices/system/cpu/cpufreq/boost", "r");
            if (f2) {
                char buf[8] = {0};
                if (fgets(buf, sizeof(buf), f2))
                    strncpy(prov->turbo_state,
                            (buf[0] == '1') ? "enabled" : "disabled",
                            sizeof(prov->turbo_state));
                else
                    strncpy(prov->turbo_state, "unknown", sizeof(prov->turbo_state));
                fclose(f2);
            } else {
                strncpy(prov->turbo_state, "unknown", sizeof(prov->turbo_state));
            }
        }
    }

    /* RAPL source label */
    strncpy(prov->rapl_source, "intel-rapl:0", sizeof(prov->rapl_source));

    /* trial_index: caller sets this before each trial */
    prov->trial_index = 0;

    return 0;
}

/*
 * Build a comma-separated string of per-core current frequencies (kHz).
 * Reads /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq for each CPU.
 * Result is written into buf (len bytes).  Cores that cannot be read are
 * recorded as "?".
 */
static void collect_per_core_freqs(char *buf, size_t len) {
    buf[0] = '\0';
    size_t off = 0;
    for (int cpu = 0; cpu < MAX_CPUS && off < len - 1; cpu++) {
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        FILE *f = fopen(path, "r");
        if (!f) break;  /* no more CPUs */
        char freq[32] = "?";
        if (!fgets(freq, sizeof(freq), f)) strncpy(freq, "?", sizeof(freq));
        fclose(f);
        size_t l = strlen(freq);
        if (l > 0 && freq[l-1] == '\n') freq[l-1] = '\0';
        int written = snprintf(buf + off, len - off, "%s%s",
                               (cpu == 0) ? "" : ";", freq);
        if (written < 0 || (size_t)written >= len - off) break;
        off += (size_t)written;
    }
    if (buf[0] == '\0') strncpy(buf, "unavailable", len);
}

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

    /* Collect run-level provenance (Rule 3) */
    if (daemon_collect_provenance(&daemon->provenance) != 0) {
        LOG_WARN("Could not collect full provenance metadata.");
    }
    LOG_INFO("Provenance: git=%s kernel=%s cpu=%s governor=%s turbo=%s",
             daemon->provenance.git_commit, daemon->provenance.kernel_version,
             daemon->provenance.cpu_model, daemon->provenance.governor,
             daemon->provenance.turbo_state);

    /* Detect hybrid vs homogeneous PMU (Step 1) */
    pmu_detect_hybrid(&daemon->hybrid);

    /* Initialize hardware topology */
    if (topology_init(&daemon->topo, config_file) != 0) {
        LOG_ERROR("Failed to initialize CPU topology");
        return -1;
    }

    /* Initialize Intel RAPL energy meter — required, no fallback */
    if (rapl_init(&daemon->rapl) != 0) {
        LOG_ERROR("FATAL: Intel RAPL energy meter initialization failed. "
                  "Cannot run evaluation without real energy measurements.");
        return -1;
    }

    /* Open telemetry CSV for plotting */
    daemon->telemetry_csv = fopen("results/daemon_telemetry.csv", "w");
    if (daemon->telemetry_csv) {
        /* Rule 3: every row must carry provenance metadata */
        fprintf(daemon->telemetry_csv,
                "git_commit,kernel_version,cpu_model,governor,turbo_state,"
                "per_core_freq_khz,rapl_source,trial_index,timestamp_iso,"
                "timestamp_sec,pid,tid,current_cpu,core_pool,"
                "ipc,mpki,delta_cycles,delta_inst,phase_shift,migrations\n");
        fflush(daemon->telemetry_csv);
    }

    daemon->running = true;
    LOG_INFO("Hybrid Scheduler Daemon initialized successfully.");
    LOG_INFO("Config: SampleInterval=%d ms, Cooldown=%d ms, Hysteresis=%d ticks, MPKI_Threshold=%.1f",
             daemon->config.sampling_interval_ms, daemon->config.cooldown_ms,
             daemon->config.hysteresis_ticks, daemon->config.mpki_threshold);
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

                /* Initialize PMU context — use hybrid-aware variant */
                if (pmu_init_thread_ctx_ex(&new_t->pmu_ctx, pid, tid,
                                           &daemon->hybrid) != 0) {
                    LOG_ERROR("FATAL: PMU init failed for TID %d. "
                              "Cannot proceed without real hardware counters.", tid);
                    /* daemon_init already printed the full diagnostic to stderr */
                    daemon_cleanup(daemon);
                    exit(1);
                }

                /* Initialize Phase Detector */
                phase_detector_init(&new_t->phase_det,
                                    daemon->config.sliding_window_size,
                                    daemon->config.phase_variance_threshold,
                                    daemon->config.phase_mean_shift_threshold);

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

        /* ================= DECISION ENGINE ================= */
        core_type_t desired_pool = t->current_pool;
        char decision_reason[64] = "STABLE";

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

        /* Log telemetry for plotting — Rule 3: include full provenance per row */
        if (daemon->telemetry_csv) {
            /* per-core frequencies snapshot */
            char freq_str[2048];
            collect_per_core_freqs(freq_str, sizeof(freq_str));

            /* ISO-8601 wall-clock timestamp */
            time_t wall_sec = time(NULL);
            struct tm *tm_utc = gmtime(&wall_sec);
            char iso_ts[32];
            strftime(iso_ts, sizeof(iso_ts), "%Y-%m-%dT%H:%M:%SZ", tm_utc);

            fprintf(daemon->telemetry_csv,
                    "%s,%s,\"%s\",%s,%s,"
                    "\"%s\",%s,%d,%s,"
                    "%.3f,%d,%d,%d,%s,"
                    "%.3f,%.2f,%.0f,%.0f,%d,%d\n",
                    daemon->provenance.git_commit,
                    daemon->provenance.kernel_version,
                    daemon->provenance.cpu_model,
                    daemon->provenance.governor,
                    daemon->provenance.turbo_state,
                    freq_str,
                    daemon->provenance.rapl_source,
                    daemon->provenance.trial_index,
                    iso_ts,
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

        /* New candidate pool detected */
        if (desired_pool != t->candidate_pool) {
            t->candidate_pool = desired_pool;
            t->candidate_consecutive_ticks = 1;
        } else {
            t->candidate_consecutive_ticks++;
        }

        /* Check cooldown timer (T_cooldown) */
        uint64_t dwell_time_ms = now_ms - t->last_migration_ms;
        if (dwell_time_ms < (uint64_t)daemon->config.cooldown_ms) {
            LOG_DEBUG("[COOLDOWN] TID %d migration to %s suppressed (dwell=%lu ms < cooldown=%d ms)",
                      t->tid, core_type_to_str(desired_pool),
                      (unsigned long)dwell_time_ms, daemon->config.cooldown_ms);
            continue;
        }

        /* Check hysteresis threshold */
        if (t->candidate_consecutive_ticks < daemon->config.hysteresis_ticks) {
            LOG_DEBUG("[HYSTERESIS] TID %d candidate %s held (tick %d/%d)",
                      t->tid, core_type_to_str(desired_pool),
                      t->candidate_consecutive_ticks, daemon->config.hysteresis_ticks);
            continue;
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
                strncpy(ev->reason, decision_reason, sizeof(ev->reason));
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
        daemon_sample_and_schedule_tick(daemon);
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
    LOG_INFO("Hybrid Scheduler Daemon shut down cleanly (Total Migrations: %d).",
             daemon->total_migrations_logged);
}
