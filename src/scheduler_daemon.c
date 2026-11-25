#include "scheduler_daemon.h"
#include <dirent.h>
#include <signal.h>
#include <sys/utsname.h>

double scheduler_compute_speedup(double ips_p, double ips_e) {
    if (ips_p <= 0.0 || ips_e <= 0.0) return 0.0;
    return ips_p / ips_e;
}

/* -------------------------------------------------------------------------
 * Rule 3: collect run-level provenance from /proc, sysfs, and git.
 * ------------------------------------------------------------------------- */
int daemon_collect_provenance(run_provenance_t *prov) {
    if (!prov) return -1;
    memset(prov, 0, sizeof(*prov));

    {
        FILE *fp = popen("git rev-parse --short HEAD 2>/dev/null", "r");
        if (fp) {
            if (!fgets(prov->git_commit, sizeof(prov->git_commit), fp))
                snprintf(prov->git_commit, sizeof(prov->git_commit), "unknown");
            else {
                size_t l = strlen(prov->git_commit);
                if (l > 0 && prov->git_commit[l - 1] == '\n') prov->git_commit[l - 1] = '\0';
            }
            pclose(fp);
        } else {
            snprintf(prov->git_commit, sizeof(prov->git_commit), "unknown");
        }
    }

    {
        struct utsname u;
        if (uname(&u) == 0)
            snprintf(prov->kernel_version, sizeof(prov->kernel_version), "%s", u.release);
        else
            snprintf(prov->kernel_version, sizeof(prov->kernel_version), "unknown");
    }

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
                        snprintf(prov->cpu_model, sizeof(prov->cpu_model), "%s", v);
                        size_t l = strlen(prov->cpu_model);
                        if (l > 0 && prov->cpu_model[l - 1] == '\n') prov->cpu_model[l - 1] = '\0';
                        found = true;
                        break;
                    }
                }
            }
            fclose(f);
        }
        if (!found) snprintf(prov->cpu_model, sizeof(prov->cpu_model), "unknown");
    }

    {
        FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
        if (f) {
            if (!fgets(prov->governor, sizeof(prov->governor), f))
                snprintf(prov->governor, sizeof(prov->governor), "unknown");
            else {
                size_t l = strlen(prov->governor);
                if (l > 0 && prov->governor[l - 1] == '\n') prov->governor[l - 1] = '\0';
            }
            fclose(f);
        } else {
            snprintf(prov->governor, sizeof(prov->governor), "unknown");
        }
    }

    {
        FILE *f = fopen("/sys/devices/system/cpu/intel_pstate/no_turbo", "r");
        if (f) {
            char buf[8] = {0};
            if (fgets(buf, sizeof(buf), f)) {
                snprintf(prov->turbo_state, sizeof(prov->turbo_state),
                         "%s", (buf[0] == '0') ? "enabled" : "disabled");
            } else {
                snprintf(prov->turbo_state, sizeof(prov->turbo_state), "unknown");
            }
            fclose(f);
        } else {
            FILE *f2 = fopen("/sys/devices/system/cpu/cpufreq/boost", "r");
            if (f2) {
                char buf[8] = {0};
                if (fgets(buf, sizeof(buf), f2))
                    snprintf(prov->turbo_state, sizeof(prov->turbo_state),
                             "%s", (buf[0] == '1') ? "enabled" : "disabled");
                else
                    snprintf(prov->turbo_state, sizeof(prov->turbo_state), "unknown");
                fclose(f2);
            } else {
                snprintf(prov->turbo_state, sizeof(prov->turbo_state), "unknown");
            }
        }
    }

    snprintf(prov->rapl_source, sizeof(prov->rapl_source), "intel-rapl:0");
    prov->trial_index = 0;
    return 0;
}

static void collect_per_core_freqs(char *buf, size_t len) {
    buf[0] = '\0';
    size_t off = 0;
    for (int cpu = 0; cpu < MAX_CPUS && off < len - 1; cpu++) {
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        FILE *f = fopen(path, "r");
        if (!f) break;
        char freq[32] = "?";
        if (!fgets(freq, sizeof(freq), f)) snprintf(freq, sizeof(freq), "?");
        fclose(f);
        size_t l = strlen(freq);
        if (l > 0 && freq[l - 1] == '\n') freq[l - 1] = '\0';
        int written = snprintf(buf + off, len - off, "%s%s",
                               (cpu == 0) ? "" : ";", freq);
        if (written < 0 || (size_t)written >= len - off) break;
        off += (size_t)written;
    }
    if (buf[0] == '\0') snprintf(buf, len, "unavailable");
}

static void update_cpu_utilization(scheduler_daemon_t *daemon) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9')
            continue;
        int cpu_id = -1;
        unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0,
                           irq = 0, softirq = 0, steal = 0;
        int n = sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                       &cpu_id, &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal);
        if (n < 5 || cpu_id < 0 || cpu_id >= MAX_CPUS) continue;

        uint64_t idle_ticks = (uint64_t)(idle + iowait);
        uint64_t total_ticks = (uint64_t)(user + nice + sys + idle + iowait + irq + softirq + steal);
        uint64_t busy_ticks = total_ticks - idle_ticks;

        if (daemon->has_cpu_stat_prev) {
            uint64_t dt = total_ticks - daemon->prev_cpu_total[cpu_id];
            uint64_t db = busy_ticks - daemon->prev_cpu_busy[cpu_id];
            daemon->cpu_busy_pct[cpu_id] = (dt > 0) ? ((double)db * 100.0 / (double)dt) : 0.0;
        }
        daemon->prev_cpu_busy[cpu_id] = busy_ticks;
        daemon->prev_cpu_total[cpu_id] = total_ticks;
    }
    fclose(f);
    daemon->has_cpu_stat_prev = true;
}

static int find_idle_core_in_pool(const scheduler_daemon_t *daemon, core_type_t dest_pool) {
    for (int c = 0; c < daemon->topo.total_cpus; c++) {
        if (topology_get_core_type(&daemon->topo, c) != dest_pool) continue;

        /* Check if any monitored thread is currently on CPU c or probing onto c */
        bool occupied = false;
        for (int i = 0; i < daemon->num_threads; i++) {
            const thread_sched_ctx_t *other = &daemon->threads[i];
            if (!other->active) continue;
            if (other->last_metrics.current_cpu == c) {
                occupied = true;
                break;
            }
            if (other->probe_state != PROBE_IDLE && other->probe_target_cpu == c) {
                occupied = true;
                break;
            }
        }
        if (occupied) continue;

        /* Also verify OS-wide utilization on CPU c is idle (< 20%) */
        if (daemon->has_cpu_stat_prev && daemon->cpu_busy_pct[c] >= 20.0) {
            continue;
        }
        return c;
    }
    return -1;
}

static speedup_cache_entry_t *lookup_speedup_cache(thread_sched_ctx_t *t, uint64_t phase_id) {
    for (int i = 0; i < MAX_PHASE_CACHE; i++) {
        if (t->speedup_cache[i].valid && t->speedup_cache[i].phase_id == phase_id) {
            return &t->speedup_cache[i];
        }
    }
    return NULL;
}

static void update_speedup_cache(thread_sched_ctx_t *t, uint64_t phase_id,
                                 double raw_speedup, double alpha, uint64_t now_ms) {
    speedup_cache_entry_t *entry = lookup_speedup_cache(t, phase_id);
    if (entry) {
        entry->speedup = alpha * raw_speedup + (1.0 - alpha) * entry->speedup;
        entry->last_updated_ms = now_ms;
        entry->sample_count++;
        return;
    }

    /* Find free or oldest slot */
    int target_idx = 0;
    uint64_t oldest_ms = UINT64_MAX;
    for (int i = 0; i < MAX_PHASE_CACHE; i++) {
        if (!t->speedup_cache[i].valid) {
            target_idx = i;
            break;
        }
        if (t->speedup_cache[i].last_updated_ms < oldest_ms) {
            oldest_ms = t->speedup_cache[i].last_updated_ms;
            target_idx = i;
        }
    }
    t->speedup_cache[target_idx].phase_id = phase_id;
    t->speedup_cache[target_idx].speedup = raw_speedup;
    t->speedup_cache[target_idx].last_updated_ms = now_ms;
    t->speedup_cache[target_idx].sample_count = 1;
    t->speedup_cache[target_idx].valid = true;
}

static void log_probe_event(scheduler_daemon_t *daemon, const thread_sched_ctx_t *t,
                            uint64_t phase_id, core_type_t src, core_type_t dst,
                            int probe_cpu, double ips_before, double ips_during,
                            double mpki, int discarded_ticks,
                            const char *status, const char *reason,
                            double now_sec) {
    if (!daemon->probe_csv) return;

    char freq_str[2048];
    collect_per_core_freqs(freq_str, sizeof(freq_str));

    time_t wall_sec = time(NULL);
    struct tm *tm_utc = gmtime(&wall_sec);
    char iso_ts[32];
    strftime(iso_ts, sizeof(iso_ts), "%Y-%m-%dT%H:%M:%SZ", tm_utc);

    fprintf(daemon->probe_csv,
            "%s,%s,\"%s\",%s,%s,"
            "\"%s\",%s,\"%s\",\"%s\",\"%s\","
            "%d,%s,"
            "%.3f,%d,%d,%" PRIu64 ",%s,%s,%d,"
            "%.1f,%.1f,%.2f,%d,%s,%s\n",
            daemon->provenance.git_commit,
            daemon->provenance.kernel_version,
            daemon->provenance.cpu_model,
            daemon->provenance.governor,
            daemon->provenance.turbo_state,
            freq_str,
            daemon->provenance.rapl_source,
            daemon->provenance.p_cores_pool,
            daemon->provenance.e_cores_pool,
            daemon->provenance.excluded_siblings,
            daemon->provenance.trial_index,
            iso_ts,
            now_sec, t->pid, t->tid, phase_id,
            core_type_to_str(src), core_type_to_str(dst), probe_cpu,
            ips_before, ips_during, mpki, discarded_ticks,
            status, reason);
    fflush(daemon->probe_csv);
}

static void parse_daemon_config(daemon_config_t *cfg, const char *path) {
    cfg->sampling_interval_ms = 250;
    cfg->mpki_threshold = 15.0;
    cfg->sliding_window_size = 10;
    cfg->phase_variance_threshold = 0.08;
    cfg->phase_mean_shift_threshold = 0.25;
    cfg->cooldown_ms = 1500;
    cfg->hysteresis_ticks = 2;
    cfg->enable_edp_optimization = true;
    cfg->p_core_power_multiplier = 2.5;
    cfg->p_cores_str[0] = '\0';
    cfg->e_cores_str[0] = '\0';

    /* Speedup & probe defaults */
    cfg->policy = SCHED_POLICY_SPEEDUP;
    cfg->probe_window_ticks = 2;
    cfg->discard_ticks = 1;
    cfg->probe_rate_cap = 0.5;
    cfg->decay_alpha = 0.5;
    cfg->estimate_ttl_ms = 15000;

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
            else if (strcmp(key, "p_cores") == 0) snprintf(cfg->p_cores_str, sizeof(cfg->p_cores_str), "%s", val);
            else if (strcmp(key, "e_cores") == 0) snprintf(cfg->e_cores_str, sizeof(cfg->e_cores_str), "%s", val);
        }
    }
    fclose(f);
}

int daemon_init(scheduler_daemon_t *daemon, const char *config_file) {
    if (!daemon) return -1;
    memset(daemon, 0, sizeof(*daemon));

    parse_daemon_config(&daemon->config, config_file);

    if (daemon_collect_provenance(&daemon->provenance) != 0) {
        LOG_WARN("Could not collect full provenance metadata.");
    }
    LOG_INFO("Provenance: git=%s kernel=%s cpu=%s governor=%s turbo=%s",
             daemon->provenance.git_commit, daemon->provenance.kernel_version,
             daemon->provenance.cpu_model, daemon->provenance.governor,
             daemon->provenance.turbo_state);

    pmu_detect_hybrid(&daemon->hybrid);

    if (topology_init(&daemon->topo, config_file) != 0) {
        LOG_ERROR("Failed to initialize CPU topology");
        return -1;
    }

    cpuset_to_str(&daemon->topo.p_core_mask, daemon->topo.total_cpus,
                  daemon->provenance.p_cores_pool, sizeof(daemon->provenance.p_cores_pool));
    cpuset_to_str(&daemon->topo.e_core_mask, daemon->topo.total_cpus,
                  daemon->provenance.e_cores_pool, sizeof(daemon->provenance.e_cores_pool));
    snprintf(daemon->provenance.excluded_siblings, sizeof(daemon->provenance.excluded_siblings), "none");
    {
        FILE *pf = fopen("config/pools.yaml", "r");
        if (pf) {
            char line[MAX_LINE_LEN];
            while (fgets(line, sizeof(line), pf)) {
                char key[64], val[128];
                if (sscanf(line, " %63[^:= ] %*[:=] \"%127[^\"]\"", key, val) == 2 ||
                    sscanf(line, " %63[^:= ] %*[:=] %127s", key, val) == 2) {
                    if (strcmp(key, "excluded_siblings") == 0) {
                        snprintf(daemon->provenance.excluded_siblings,
                                 sizeof(daemon->provenance.excluded_siblings), "%s", val);
                    }
                }
            }
            fclose(pf);
        }
    }

    if (rapl_init(&daemon->rapl) != 0) {
        LOG_ERROR("FATAL: Intel RAPL energy meter initialization failed. "
                  "Cannot run evaluation without real energy measurements.");
        return -1;
    }

    daemon->telemetry_csv = fopen("results/daemon_telemetry.csv", "w");
    if (daemon->telemetry_csv) {
        fprintf(daemon->telemetry_csv,
                "git_commit,kernel_version,cpu_model,governor,turbo_state,"
                "per_core_freq_khz,rapl_source,p_cores,e_cores,excluded_siblings,"
                "trial_index,timestamp_iso,"
                "timestamp_sec,pid,tid,current_cpu,core_pool,"
                "ipc,mpki,instr_per_sec,delta_cycles,delta_inst,"
                "phase_id,phase_shift,speedup_estimate,estimate_age_ms,"
                "chosen_pool,reason,migrations\n");
        fflush(daemon->telemetry_csv);
    }

    daemon->probe_csv = fopen("results/probe_log.csv", "w");
    if (daemon->probe_csv) {
        fprintf(daemon->probe_csv,
                "git_commit,kernel_version,cpu_model,governor,turbo_state,"
                "per_core_freq_khz,rapl_source,p_cores,e_cores,excluded_siblings,"
                "trial_index,timestamp_iso,"
                "timestamp_sec,pid,tid,phase_id,source_pool,dest_pool,probe_cpu,"
                "instr_per_sec_before,instr_per_sec_during,mpki,discarded_ticks,"
                "status,reason\n");
        fflush(daemon->probe_csv);
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
                memset(new_t, 0, sizeof(*new_t));
                new_t->pid = pid;
                new_t->tid = tid;
                new_t->active = true;
                new_t->probe_state = PROBE_IDLE;
                new_t->probe_target_cpu = -1;
                new_t->last_migration_ms = (uint64_t)get_time_ms();

                if (pmu_init_thread_ctx_ex(&new_t->pmu_ctx, pid, tid,
                                           &daemon->hybrid) != 0) {
                    LOG_ERROR("FATAL: PMU init failed for TID %d. "
                              "Cannot proceed without real hardware counters.", tid);
                    daemon_cleanup(daemon);
                    exit(1);
                }

                phase_detector_init(&new_t->phase_det,
                                    daemon->config.sliding_window_size,
                                    daemon->config.phase_variance_threshold,
                                    daemon->config.phase_mean_shift_threshold);

                int cur_cpu = pmu_get_thread_cpu(pid, tid);
                new_t->current_pool = topology_get_core_type(&daemon->topo, cur_cpu);
                if (new_t->current_pool == CORE_TYPE_UNKNOWN) {
                    new_t->current_pool = CORE_TYPE_P;
                    sched_setaffinity(tid, sizeof(cpu_set_t), &daemon->topo.p_core_mask);
                }
                new_t->candidate_pool = new_t->current_pool;

                LOG_INFO("[THREAD ATTACH] Monitored PID %d -> TID %d attached (Initial Core %d: %s)",
                         pid, tid, cur_cpu, core_type_to_str(new_t->current_pool));
            }
        }
    }
}

typedef struct {
    int    thread_idx;
    double speedup;
} rank_item_t;

static int cmp_rank_desc(const void *a, const void *b) {
    const rank_item_t *ra = (const rank_item_t *)a;
    const rank_item_t *rb = (const rank_item_t *)b;
    if (rb->speedup > ra->speedup) return 1;
    if (rb->speedup < ra->speedup) return -1;
    return 0;
}

void daemon_sample_and_schedule_tick(scheduler_daemon_t *daemon) {
    if (!daemon) return;

    uint64_t now_ms = (uint64_t)get_time_ms();
    double now_sec = get_time_sec();

    update_cpu_utilization(daemon);
    daemon_discover_threads(daemon);

    /* Phase 1: Sample all active threads and advance probe state machines */
    for (int i = 0; i < daemon->num_threads; i++) {
        thread_sched_ctx_t *t = &daemon->threads[i];
        if (!t->active) continue;

        pmu_metrics_t metrics;
        if (pmu_sample_thread(&t->pmu_ctx, &metrics) != 0) {
            pmu_close_thread_ctx(&t->pmu_ctx);
            t->active = false;
            LOG_DEBUG("[THREAD DETACH] TID %d terminated", t->tid);
            continue;
        }

        phase_stat_t pstats;
        bool is_phase_shift = phase_detector_push(&t->phase_det, metrics.ipc, metrics.mpki, &pstats);
        t->last_metrics = metrics;
        t->last_phase_stats = pstats;

        double ips = (metrics.delta_time_sec > 0.0)
                     ? (metrics.delta_instructions / metrics.delta_time_sec) : 0.0;

        /* Advance active probe state machine if probing */
        if (t->probe_state == PROBE_WARMUP) {
            if (is_phase_shift) {
                cpu_set_t *src_mask = (t->probe_source_pool == CORE_TYPE_P)
                                      ? &daemon->topo.p_core_mask : &daemon->topo.e_core_mask;
                sched_setaffinity(t->tid, sizeof(cpu_set_t), src_mask);
                t->current_pool = t->probe_source_pool;
                t->last_migration_ms = now_ms;
                log_probe_event(daemon, t, t->probe_phase_id,
                                t->probe_source_pool, t->probe_dest_pool,
                                t->probe_target_cpu, t->probe_before_ips, 0.0,
                                metrics.mpki, daemon->config.discard_ticks,
                                "invalidated", "PHASE_CHANGE_MID_PROBE_WARMUP", now_sec);
                t->probe_state = PROBE_IDLE;
                t->probe_target_cpu = -1;
                t->current_ips_ema = 0.0;
                t->current_ips_samples = 0;
                continue;
            }
            t->probe_ticks_remaining--;
            if (t->probe_ticks_remaining <= 0) {
                t->probe_state = PROBE_MEASURING;
                t->probe_ticks_remaining = daemon->config.probe_window_ticks;
                t->probe_accum_inst = 0.0;
                t->probe_accum_sec = 0.0;
            }
            continue;
        }

        if (t->probe_state == PROBE_MEASURING) {
            if (is_phase_shift) {
                cpu_set_t *src_mask = (t->probe_source_pool == CORE_TYPE_P)
                                      ? &daemon->topo.p_core_mask : &daemon->topo.e_core_mask;
                sched_setaffinity(t->tid, sizeof(cpu_set_t), src_mask);
                t->current_pool = t->probe_source_pool;
                t->last_migration_ms = now_ms;
                log_probe_event(daemon, t, t->probe_phase_id,
                                t->probe_source_pool, t->probe_dest_pool,
                                t->probe_target_cpu, t->probe_before_ips, 0.0,
                                metrics.mpki, daemon->config.discard_ticks,
                                "invalidated", "PHASE_CHANGE_MID_PROBE", now_sec);
                t->probe_state = PROBE_IDLE;
                t->probe_target_cpu = -1;
                t->current_ips_ema = 0.0;
                t->current_ips_samples = 0;
                continue;
            }

            t->probe_accum_inst += metrics.delta_instructions;
            t->probe_accum_sec  += metrics.delta_time_sec;
            t->probe_last_mpki   = metrics.mpki;
            t->probe_ticks_remaining--;

            if (t->probe_ticks_remaining <= 0) {
                double ips_during = (t->probe_accum_sec > 0.0)
                                    ? (t->probe_accum_inst / t->probe_accum_sec) : 0.0;
                double ips_p = (t->probe_source_pool == CORE_TYPE_P) ? t->probe_before_ips : ips_during;
                double ips_e = (t->probe_source_pool == CORE_TYPE_E) ? t->probe_before_ips : ips_during;
                double raw_speedup = scheduler_compute_speedup(ips_p, ips_e);

                if (raw_speedup > 0.0) {
                    update_speedup_cache(t, t->probe_phase_id, raw_speedup,
                                         daemon->config.decay_alpha, now_ms);
                    log_probe_event(daemon, t, t->probe_phase_id,
                                    t->probe_source_pool, t->probe_dest_pool,
                                    t->probe_target_cpu, t->probe_before_ips, ips_during,
                                    t->probe_last_mpki, daemon->config.discard_ticks,
                                    "valid", "OK", now_sec);
                    LOG_INFO("[PROBE VALID] TID %d phase=%" PRIu64 " %s->%s (CPU %d) "
                             "ips_before=%.1f ips_during=%.1f raw_speedup=%.3fx",
                             t->tid, t->probe_phase_id,
                             core_type_to_str(t->probe_source_pool),
                             core_type_to_str(t->probe_dest_pool),
                             t->probe_target_cpu, t->probe_before_ips, ips_during, raw_speedup);
                } else {
                    log_probe_event(daemon, t, t->probe_phase_id,
                                    t->probe_source_pool, t->probe_dest_pool,
                                    t->probe_target_cpu, t->probe_before_ips, ips_during,
                                    t->probe_last_mpki, daemon->config.discard_ticks,
                                    "invalidated", "ZERO_THROUGHPUT", now_sec);
                }

                /* Migrate back to source pool */
                cpu_set_t *src_mask = (t->probe_source_pool == CORE_TYPE_P)
                                      ? &daemon->topo.p_core_mask : &daemon->topo.e_core_mask;
                sched_setaffinity(t->tid, sizeof(cpu_set_t), src_mask);
                t->current_pool = t->probe_source_pool;
                t->last_migration_ms = now_ms;
                t->probe_target_cpu = -1;
                t->total_probes++;

                if (daemon->config.discard_ticks > 0) {
                    t->probe_state = PROBE_RETURN_WARMUP;
                    t->probe_ticks_remaining = daemon->config.discard_ticks;
                } else {
                    t->probe_state = PROBE_IDLE;
                }
            }
            continue;
        }

        if (t->probe_state == PROBE_RETURN_WARMUP) {
            t->probe_ticks_remaining--;
            if (t->probe_ticks_remaining <= 0) {
                t->probe_state = PROBE_IDLE;
            }
            continue;
        }

        /* Normal (PROBE_IDLE) tick: update baseline throughput EMA on current pool */
        if (is_phase_shift) {
            t->current_ips_ema = ips;
            t->current_ips_samples = 1;
        } else if (ips > 0.0) {
            if (t->current_ips_samples == 0) {
                t->current_ips_ema = ips;
            } else {
                t->current_ips_ema = daemon->config.decay_alpha * ips
                                   + (1.0 - daemon->config.decay_alpha) * t->current_ips_ema;
            }
            t->current_ips_samples++;
        }

        /* Under --policy speedup, trigger probe if no fresh estimate for current phase */
        if (daemon->config.policy == SCHED_POLICY_SPEEDUP && t->current_ips_samples >= 1) {
            speedup_cache_entry_t *cached = lookup_speedup_cache(t, pstats.phase_id);
            bool need_probe = (!cached) || ((now_ms - cached->last_updated_ms) > daemon->config.estimate_ttl_ms);

            if (need_probe) {
                double min_interval_ms = (daemon->config.probe_rate_cap > 0.0)
                                         ? (1000.0 / daemon->config.probe_rate_cap) : 0.0;
                core_type_t dest_pool = (t->current_pool == CORE_TYPE_P) ? CORE_TYPE_E : CORE_TYPE_P;

                if (min_interval_ms > 0.0 && (now_ms - t->last_probe_ms) < (uint64_t)min_interval_ms) {
                    if ((now_ms - t->last_skip_log_ms) >= (uint64_t)min_interval_ms) {
                        log_probe_event(daemon, t, pstats.phase_id,
                                        t->current_pool, dest_pool, -1,
                                        t->current_ips_ema, 0.0, metrics.mpki,
                                        daemon->config.discard_ticks,
                                        "skipped", "RATE_CAPPED", now_sec);
                        t->last_skip_log_ms = now_ms;
                    }
                } else {
                    int idle_cpu = find_idle_core_in_pool(daemon, dest_pool);
                    if (idle_cpu < 0) {
                        log_probe_event(daemon, t, pstats.phase_id,
                                        t->current_pool, dest_pool, -1,
                                        t->current_ips_ema, 0.0, metrics.mpki,
                                        daemon->config.discard_ticks,
                                        "skipped", "NO_IDLE_CORE_IN_DEST_POOL", now_sec);
                        t->last_probe_ms = now_ms;
                    } else {
                        cpu_set_t probe_mask;
                        CPU_ZERO(&probe_mask);
                        CPU_SET(idle_cpu, &probe_mask);
                        if (sched_setaffinity(t->tid, sizeof(cpu_set_t), &probe_mask) == 0) {
                            t->probe_source_pool = t->current_pool;
                            t->probe_dest_pool = dest_pool;
                            t->probe_target_cpu = idle_cpu;
                            t->probe_phase_id = pstats.phase_id;
                            t->probe_before_ips = t->current_ips_ema;
                            t->probe_accum_inst = 0.0;
                            t->probe_accum_sec = 0.0;
                            t->last_probe_ms = now_ms;
                            t->current_pool = dest_pool;
                            t->last_migration_ms = now_ms;
                            if (daemon->config.discard_ticks > 0) {
                                t->probe_state = PROBE_WARMUP;
                                t->probe_ticks_remaining = daemon->config.discard_ticks;
                            } else {
                                t->probe_state = PROBE_MEASURING;
                                t->probe_ticks_remaining = daemon->config.probe_window_ticks;
                            }
                            LOG_DEBUG("[PROBE START] TID %d phase=%" PRIu64 " %s -> %s (CPU %d), discard=%d",
                                      t->tid, pstats.phase_id,
                                      core_type_to_str(t->probe_source_pool),
                                      core_type_to_str(dest_pool), idle_cpu,
                                      daemon->config.discard_ticks);
                        } else {
                            log_probe_event(daemon, t, pstats.phase_id,
                                            t->current_pool, dest_pool, idle_cpu,
                                            t->current_ips_ema, 0.0, metrics.mpki,
                                            daemon->config.discard_ticks,
                                            "skipped", "SETAFFINITY_FAILED", now_sec);
                        }
                    }
                }
            }
        }
    }

    /* Phase 2: Compute pool ranking for --policy speedup */
    core_type_t ranked_desired[MAX_THREADS];
    for (int i = 0; i < daemon->num_threads; i++) {
        ranked_desired[i] = daemon->threads[i].current_pool;
    }

    if (daemon->config.policy == SCHED_POLICY_SPEEDUP) {
        rank_item_t items[MAX_THREADS];
        int n_items = 0;
        for (int i = 0; i < daemon->num_threads; i++) {
            thread_sched_ctx_t *t = &daemon->threads[i];
            if (!t->active || t->probe_state != PROBE_IDLE) continue;
            speedup_cache_entry_t *c = lookup_speedup_cache(t, t->last_phase_stats.phase_id);
            if (c && c->valid) {
                items[n_items].thread_idx = i;
                items[n_items].speedup = c->speedup;
                n_items++;
            }
        }
        qsort(items, (size_t)n_items, sizeof(rank_item_t), cmp_rank_desc);

        int p_slots = daemon->topo.num_p_cores;
        int p_assigned = 0;
        for (int k = 0; k < n_items; k++) {
            int idx = items[k].thread_idx;
            if (p_assigned < p_slots && items[k].speedup > 1.05) {
                ranked_desired[idx] = CORE_TYPE_P;
                p_assigned++;
            } else {
                ranked_desired[idx] = CORE_TYPE_E;
            }
        }
    }

    /* Phase 3: Decision logging & anti-thrashing execution */
    for (int i = 0; i < daemon->num_threads; i++) {
        thread_sched_ctx_t *t = &daemon->threads[i];
        if (!t->active || t->probe_state != PROBE_IDLE) continue;

        pmu_metrics_t metrics = t->last_metrics;
        phase_stat_t pstats = t->last_phase_stats;
        double ips = (metrics.delta_time_sec > 0.0)
                     ? (metrics.delta_instructions / metrics.delta_time_sec) : 0.0;

        speedup_cache_entry_t *cached = lookup_speedup_cache(t, pstats.phase_id);
        double speedup_est = cached ? cached->speedup : 0.0;
        long long est_age_ms = cached ? (long long)(now_ms - cached->last_updated_ms) : -1;

        core_type_t desired_pool = t->current_pool;
        char decision_reason[64] = "STABLE";

        if (daemon->config.policy == SCHED_POLICY_SPEEDUP) {
            if (!cached) {
                desired_pool = t->current_pool;
                snprintf(decision_reason, sizeof(decision_reason), "AWAITING_PROBE");
            } else {
                desired_pool = ranked_desired[i];
                snprintf(decision_reason, sizeof(decision_reason),
                         "%s(S=%.2f)",
                         (desired_pool == CORE_TYPE_P) ? "SPEEDUP_RANK_P" : "SPEEDUP_RANK_E",
                         speedup_est);
            }
        } else {
            /* Ablation policy: --policy mpki */
            if (metrics.mpki >= daemon->config.mpki_threshold) {
                desired_pool = CORE_TYPE_E;
                snprintf(decision_reason, sizeof(decision_reason),
                         "MEM_STALL_GUARD(MPKI=%.1f>%.1f)", metrics.mpki, daemon->config.mpki_threshold);
            } else if (metrics.mpki < 5.0 && metrics.ipc >= 1.2) {
                desired_pool = CORE_TYPE_P;
                snprintf(decision_reason, sizeof(decision_reason),
                         "COMPUTE_P_BOOST(IPC=%.2f)", metrics.ipc);
            } else if (metrics.ipc < 0.6 && daemon->config.enable_edp_optimization) {
                desired_pool = CORE_TYPE_E;
                snprintf(decision_reason, sizeof(decision_reason),
                         "EDP_OPTIMAL_E(IPC=%.2f)", metrics.ipc);
            }
        }

        if (daemon->telemetry_csv) {
            char freq_str[2048];
            collect_per_core_freqs(freq_str, sizeof(freq_str));

            time_t wall_sec = time(NULL);
            struct tm *tm_utc = gmtime(&wall_sec);
            char iso_ts[32];
            strftime(iso_ts, sizeof(iso_ts), "%Y-%m-%dT%H:%M:%SZ", tm_utc);

            fprintf(daemon->telemetry_csv,
                    "%s,%s,\"%s\",%s,%s,"
                    "\"%s\",%s,\"%s\",\"%s\",\"%s\","
                    "%d,%s,"
                    "%.3f,%d,%d,%d,%s,"
                    "%.3f,%.2f,%.1f,%.0f,%.0f,"
                    "%" PRIu64 ",%d,%.4f,%lld,"
                    "%s,%s,%d\n",
                    daemon->provenance.git_commit,
                    daemon->provenance.kernel_version,
                    daemon->provenance.cpu_model,
                    daemon->provenance.governor,
                    daemon->provenance.turbo_state,
                    freq_str,
                    daemon->provenance.rapl_source,
                    daemon->provenance.p_cores_pool,
                    daemon->provenance.e_cores_pool,
                    daemon->provenance.excluded_siblings,
                    daemon->provenance.trial_index,
                    iso_ts,
                    now_sec, t->pid, t->tid, metrics.current_cpu,
                    core_type_to_str(t->current_pool),
                    metrics.ipc, metrics.mpki, ips,
                    metrics.delta_cycles, metrics.delta_instructions,
                    pstats.phase_id, pstats.is_phase_shifting ? 1 : 0,
                    speedup_est, est_age_ms,
                    core_type_to_str(desired_pool), decision_reason, t->total_migrations);
            fflush(daemon->telemetry_csv);
        }

        /* Anti-thrashing state machine */
        if (desired_pool == t->current_pool) {
            t->candidate_consecutive_ticks = 0;
            t->candidate_pool = t->current_pool;
            continue;
        }

        if (desired_pool != t->candidate_pool) {
            t->candidate_pool = desired_pool;
            t->candidate_consecutive_ticks = 1;
        } else {
            t->candidate_consecutive_ticks++;
        }

        uint64_t dwell_time_ms = now_ms - t->last_migration_ms;
        if (dwell_time_ms < (uint64_t)daemon->config.cooldown_ms) {
            LOG_DEBUG("[COOLDOWN] TID %d migration to %s suppressed (dwell=%lu ms < cooldown=%d ms)",
                      t->tid, core_type_to_str(desired_pool),
                      (unsigned long)dwell_time_ms, daemon->config.cooldown_ms);
            continue;
        }

        if (t->candidate_consecutive_ticks < daemon->config.hysteresis_ticks) {
            LOG_DEBUG("[HYSTERESIS] TID %d candidate %s held (tick %d/%d)",
                      t->tid, core_type_to_str(desired_pool),
                      t->candidate_consecutive_ticks, daemon->config.hysteresis_ticks);
            continue;
        }

        cpu_set_t *target_mask = (desired_pool == CORE_TYPE_P)
                                 ? &daemon->topo.p_core_mask : &daemon->topo.e_core_mask;
        int ret = sched_setaffinity(t->tid, sizeof(cpu_set_t), target_mask);
        if (ret == 0) {
            int prev_cpu = metrics.current_cpu;
            core_type_t prev_pool = t->current_pool;
            t->current_pool = desired_pool;
            t->last_migration_ms = now_ms;
            t->total_migrations++;
            t->candidate_consecutive_ticks = 0;

            LOG_INFO(COLOR_MAGENTA "[MIGRATION #%d] TID %d moved: %s (CPU %d) -> %s | Reason: %s (IPC=%.2f, MPKI=%.1f, S=%.2f)" COLOR_RESET,
                     t->total_migrations, t->tid,
                     core_type_to_str(prev_pool), prev_cpu,
                     core_type_to_str(desired_pool), decision_reason,
                     metrics.ipc, metrics.mpki, speedup_est);

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
    if (daemon->probe_csv) {
        fclose(daemon->probe_csv);
        daemon->probe_csv = NULL;
    }
    LOG_INFO("Hybrid Scheduler Daemon shut down cleanly (Total Migrations: %d).",
             daemon->total_migrations_logged);
}
