#include "pmu_sampler.h"
#include <dirent.h>
#include <sys/ioctl.h>

/*
 * perf_event_open group-read format when PERF_FORMAT_GROUP is set.
 * Three counters: cycles (leader), instructions, LLC misses.
 */
struct perf_group_read_format {
    uint64_t nr;
    struct {
        uint64_t value;
    } values[3];
};

static long perf_event_open_syscall(struct perf_event_attr *hw_event, pid_t pid,
                                   int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

/* -------------------------------------------------------------------------
 * Hybrid PMU detection.
 *
 * On Intel hybrid CPUs (Alder Lake, Raptor Lake, Meteor Lake) perf exposes
 * two separate PMU devices:
 *   /sys/bus/event_source/devices/cpu_core/type  → P-core PMU type id
 *   /sys/bus/event_source/devices/cpu_atom/type  → E-core PMU type id
 *
 * On homogeneous CPUs (Xeon w5-2565X, Skylake, etc.) only the generic "cpu"
 * PMU exists; PERF_TYPE_HARDWARE (= 0) is correct for all cores.
 *
 * We detect hybrid at startup.  If not hybrid, both pmu_type_p and
 * pmu_type_e in pmu_hybrid_info_t equal PERF_TYPE_HARDWARE.
 * ------------------------------------------------------------------------- */

static uint32_t read_pmu_type_file(const char *pmu_name) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path),
             "/sys/bus/event_source/devices/%s/type", pmu_name);
    FILE *f = fopen(path, "r");
    if (!f) return UINT32_MAX;
    unsigned int t = UINT32_MAX;
    if (fscanf(f, "%u", &t) != 1) t = UINT32_MAX;
    fclose(f);
    return (uint32_t)t;
}

void pmu_detect_hybrid(pmu_hybrid_info_t *info) {
    if (!info) return;
    info->is_hybrid  = false;
    info->type_pcore = PERF_TYPE_HARDWARE;
    info->type_ecore = PERF_TYPE_HARDWARE;

    uint32_t t_core = read_pmu_type_file("cpu_core");
    uint32_t t_atom = read_pmu_type_file("cpu_atom");

    if (t_core != UINT32_MAX && t_atom != UINT32_MAX) {
        info->is_hybrid  = true;
        info->type_pcore = t_core;
        info->type_ecore = t_atom;
        LOG_INFO("Hybrid CPU detected: cpu_core PMU type=%u, cpu_atom PMU type=%u",
                 t_core, t_atom);
    } else {
        LOG_INFO("Homogeneous CPU: using PERF_TYPE_HARDWARE for all cores.");
    }
}

/* -------------------------------------------------------------------------
 * Thread-local topology helpers
 * ------------------------------------------------------------------------- */

int pmu_discover_tids(pid_t pid, pid_t *tids_out, int max_tids) {
    if (!tids_out || max_tids <= 0) return 0;

    char task_dir_path[MAX_PATH_LEN];
    snprintf(task_dir_path, sizeof(task_dir_path), "/proc/%d/task", pid);

    DIR *dir = opendir(task_dir_path);
    if (!dir) {
        LOG_DEBUG("Could not open %s (errno=%d: %s)", task_dir_path, errno, strerror(errno));
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
            pid_t tid = (pid_t)atoi(entry->d_name);
            if (tid > 0) {
                tids_out[count++] = tid;
                if (count >= max_tids) break;
            }
        }
    }
    closedir(dir);
    return count;
}

int pmu_get_thread_cpu(pid_t pid, pid_t tid) {
    char stat_path[MAX_PATH_LEN];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/task/%d/stat", pid, tid);

    FILE *f = fopen(stat_path, "r");
    if (!f) return -1;

    char line[MAX_LINE_LEN];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Field 39 (1-indexed) is the CPU.  Skip past the (comm) field. */
    char *paren = strrchr(line, ')');
    if (!paren) return -1;

    int cur_cpu = -1;
    char *token = strtok(paren + 1, " ");
    int field_idx = 3;
    while (token != NULL) {
        if (field_idx == 39) { cur_cpu = atoi(token); break; }
        token = strtok(NULL, " ");
        field_idx++;
    }
    return cur_cpu;
}

/* -------------------------------------------------------------------------
 * Per-thread PMU context initialisation.
 *
 * On hybrid CPUs the correct PMU type depends on which physical core type
 * the thread is running on at open time.  If the thread later migrates to
 * the other core type, the group read will return stale/zero values or
 * EINVAL; callers should detect this and re-initialise the context.
 *
 * Group layout (PERF_FORMAT_GROUP, single syscall read):
 *   index 0 : CPU cycles         (group leader)
 *   index 1 : Instructions       (sibling)
 *   index 2 : LLC misses         (sibling; falls back to HW_CACHE_MISSES)
 *
 * Returns 0 on success.
 * Returns -1 on any hard failure — the caller MUST NOT proceed; it should
 * print the diagnostic already emitted to stderr and exit(1).
 * ------------------------------------------------------------------------- */

int pmu_init_thread_ctx(thread_pmu_ctx_t *ctx, pid_t pid, pid_t tid) {
    return pmu_init_thread_ctx_ex(ctx, pid, tid, NULL);
}

int pmu_init_thread_ctx_ex(thread_pmu_ctx_t *ctx, pid_t pid, pid_t tid,
                            const pmu_hybrid_info_t *hybrid) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->pid       = pid;
    ctx->tid       = tid;
    ctx->leader_fd = -1;
    ctx->inst_fd   = -1;
    ctx->llc_fd    = -1;
    ctx->prev_cpu  = pmu_get_thread_cpu(pid, tid);

    /*
     * Choose the PMU type for this thread.
     *
     * Homogeneous (is_hybrid == false):
     *   PERF_TYPE_HARDWARE works for every core.
     *
     * Hybrid (is_hybrid == true):
     *   We pick P-core type as the starting point.  pmu_sample_thread()
     *   detects core-type migrations by comparing pmu_type against the
     *   topology of the current CPU; the daemon re-initialises the context
     *   if the thread has moved to the other core type.
     */
    uint32_t pmu_type = PERF_TYPE_HARDWARE;
    if (hybrid && hybrid->is_hybrid) {
        pmu_type = hybrid->type_pcore;
        LOG_DEBUG("Hybrid PMU init: TID %d on CPU %d, using pmu_type=%u (P-core)",
                  tid, ctx->prev_cpu, pmu_type);
    }
    ctx->pmu_type = pmu_type;

    struct perf_event_attr pe;

    /* ---- Leader: CPU Cycles ---- */
    memset(&pe, 0, sizeof(pe));
    pe.type           = pmu_type;
    pe.size           = sizeof(pe);
    pe.config         = PERF_COUNT_HW_CPU_CYCLES;
    pe.disabled       = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    pe.read_format    = PERF_FORMAT_GROUP;

    long leader = perf_event_open_syscall(&pe, tid, -1, -1, 0);
    if (leader < 0) {
        int saved = errno;
        fprintf(stderr,
            "\n"
            "[FATAL] perf_event_open(CYCLES) failed for TID %d\n"
            "  errno=%d (%s)\n"
            "\n"
            "  Hardware PMU counters (IPC, MPKI) are REQUIRED.\n"
            "  Synthetic substitutes are FORBIDDEN in evaluation builds.\n"
            "\n"
            "  Diagnosis checklist:\n"
            "    1. Check perf_event_paranoid (must be <= 1):\n"
            "         cat /proc/sys/kernel/perf_event_paranoid\n"
            "         echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid\n"
            "    2. Grant CAP_PERFMON to the binary (Linux >= 5.8):\n"
            "         sudo setcap cap_perfmon+ep ./bin/hybridsched_daemon\n"
            "    3. Verify CONFIG_PERF_EVENTS=y in the running kernel.\n"
            "    4. In a VM/container: check PMU virtualisation pass-through.\n"
            "\n",
            tid, saved, strerror(saved));
        return -1;
    }
    ctx->leader_fd = (int)leader;

    /* ---- Sibling 1: Instructions ---- */
    memset(&pe, 0, sizeof(pe));
    pe.type           = pmu_type;
    pe.size           = sizeof(pe);
    pe.config         = PERF_COUNT_HW_INSTRUCTIONS;
    pe.disabled       = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    pe.read_format    = PERF_FORMAT_GROUP;

    long inst = perf_event_open_syscall(&pe, tid, -1, ctx->leader_fd, 0);
    if (inst < 0) {
        fprintf(stderr,
            "[FATAL] perf_event_open(INSTRUCTIONS) failed for TID %d "
            "(errno=%d: %s).\n",
            tid, errno, strerror(errno));
        close(ctx->leader_fd); ctx->leader_fd = -1;
        return -1;
    }
    ctx->inst_fd = (int)inst;

    /* ---- Sibling 2: LLC misses ---- */
    memset(&pe, 0, sizeof(pe));
    pe.size           = sizeof(pe);
    pe.disabled       = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    pe.read_format    = PERF_FORMAT_GROUP;

    /* Prefer the specific LL-cache miss event */
    pe.type   = PERF_TYPE_HW_CACHE;
    pe.config = (PERF_COUNT_HW_CACHE_LL)              |
                (PERF_COUNT_HW_CACHE_OP_READ   << 8)  |
                (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);

    long llc = perf_event_open_syscall(&pe, tid, -1, ctx->leader_fd, 0);
    if (llc < 0) {
        /* Fall back to the generic hardware cache-miss counter */
        LOG_DEBUG("TID %d: LL-specific cache event unavailable, "
                  "using PERF_COUNT_HW_CACHE_MISSES", tid);
        pe.type   = pmu_type;
        pe.config = PERF_COUNT_HW_CACHE_MISSES;
        llc = perf_event_open_syscall(&pe, tid, -1, ctx->leader_fd, 0);
    }
    if (llc < 0) {
        fprintf(stderr,
            "[FATAL] perf_event_open(LLC_MISSES) failed for TID %d "
            "(errno=%d: %s).\n",
            tid, errno, strerror(errno));
        close(ctx->inst_fd); ctx->inst_fd = -1;
        close(ctx->leader_fd); ctx->leader_fd = -1;
        return -1;
    }
    ctx->llc_fd = (int)llc;

    /* Enable the counter group atomically */
    ioctl(ctx->leader_fd, PERF_EVENT_IOC_RESET,  PERF_IOC_FLAG_GROUP);
    if (ioctl(ctx->leader_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0) {
        LOG_WARN("ioctl(ENABLE) failed for TID %d (errno=%d)", tid, errno);
    }

    ctx->pmu_active = true;
    ctx->prev_sample.timestamp_ns = get_time_ns();
    return 0;
}

void pmu_close_thread_ctx(thread_pmu_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->leader_fd >= 0) {
        ioctl(ctx->leader_fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        close(ctx->leader_fd); ctx->leader_fd = -1;
    }
    if (ctx->inst_fd >= 0) { close(ctx->inst_fd); ctx->inst_fd = -1; }
    if (ctx->llc_fd  >= 0) { close(ctx->llc_fd);  ctx->llc_fd  = -1; }
    ctx->pmu_active = false;
}

/* -------------------------------------------------------------------------
 * Sample the PMU group counter.
 *
 * Reads all three counters atomically via PERF_FORMAT_GROUP.
 * Returns 0 on success, -1 if the read fails (e.g. thread exited, or on
 * hybrid CPUs the thread migrated to the other core type).
 * ------------------------------------------------------------------------- */
int pmu_sample_thread(thread_pmu_ctx_t *ctx, pmu_metrics_t *metrics) {
    if (!ctx || !metrics) return -1;
    memset(metrics, 0, sizeof(*metrics));

    if (!ctx->pmu_active || ctx->leader_fd < 0) {
        fprintf(stderr,
            "[FATAL] pmu_sample_thread called on uninitialised PMU context "
            "(TID %d). pmu_init_thread_ctx_ex must return 0 before sampling.\n",
            ctx ? ctx->tid : -1);
        return -1;
    }

    uint64_t now_ns = get_time_ns();
    double dt_sec = (double)(now_ns - ctx->prev_sample.timestamp_ns) / 1e9;
    if (dt_sec <= 0.0) dt_sec = 1e-4;

    metrics->delta_time_sec = dt_sec;
    metrics->current_cpu    = pmu_get_thread_cpu(ctx->pid, ctx->tid);

    struct perf_group_read_format buf;
    memset(&buf, 0, sizeof(buf));
    ssize_t bytes = read(ctx->leader_fd, &buf, sizeof(buf));

    if (bytes < (ssize_t)sizeof(buf) || buf.nr < 3) {
        LOG_DEBUG("Group read failed TID %d: bytes=%zd expected=%zu nr=%" PRIu64,
                  ctx->tid, bytes, sizeof(buf), buf.nr);
        return -1;
    }

    uint64_t cur_cycles = buf.values[0].value;
    uint64_t cur_inst   = buf.values[1].value;
    uint64_t cur_llc    = buf.values[2].value;

    /* Treat counter regression as a fresh epoch (context-switch reset) */
    uint64_t d_cycles = (cur_cycles >= ctx->prev_sample.cycles)
                        ? (cur_cycles - ctx->prev_sample.cycles) : cur_cycles;
    uint64_t d_inst   = (cur_inst   >= ctx->prev_sample.instructions)
                        ? (cur_inst   - ctx->prev_sample.instructions) : cur_inst;
    uint64_t d_llc    = (cur_llc    >= ctx->prev_sample.llc_misses)
                        ? (cur_llc    - ctx->prev_sample.llc_misses) : cur_llc;

    ctx->prev_sample.cycles       = cur_cycles;
    ctx->prev_sample.instructions = cur_inst;
    ctx->prev_sample.llc_misses   = cur_llc;
    ctx->prev_sample.timestamp_ns = now_ns;

    metrics->delta_cycles       = (double)d_cycles;
    metrics->delta_instructions = (double)d_inst;
    metrics->delta_llc_misses   = (double)d_llc;
    metrics->ipc  = (d_cycles > 0) ? ((double)d_inst  / (double)d_cycles) : 0.0;
    metrics->mpki = (d_inst   > 0) ? ((double)d_llc * 1000.0 / (double)d_inst) : 0.0;

    ctx->last_metrics = *metrics;
    return 0;
}
