#include "pmu_sampler.h"
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* Group read format structure when read_format = PERF_FORMAT_GROUP */
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

    /* The processor is field 39 (1-indexed) in /proc/[pid]/task/[tid]/stat.
       Field 2 is (comm) which can contain spaces/parentheses.
       Find the last closing parenthesis to skip comm safely. */
    char *paren = strrchr(line, ')');
    if (!paren) return -1;

    int cur_cpu = -1;
    /* After ')', fields start from 3 (state) */
    char *token = strtok(paren + 1, " ");
    int field_idx = 3;
    while (token != NULL) {
        if (field_idx == 39) {
            cur_cpu = atoi(token);
            break;
        }
        token = strtok(NULL, " ");
        field_idx++;
    }
    return cur_cpu;
}


int pmu_init_thread_ctx(thread_pmu_ctx_t *ctx, pid_t pid, pid_t tid) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->pid = pid;
    ctx->tid = tid;
    ctx->leader_fd = -1;
    ctx->inst_fd = -1;
    ctx->llc_fd = -1;
    ctx->prev_cpu = pmu_get_thread_cpu(pid, tid);

    struct perf_event_attr pe;

    /* 1. Group Leader: CPU Cycles */
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.read_format = PERF_FORMAT_GROUP;

    long leader = perf_event_open_syscall(&pe, tid, -1, -1, 0);
    if (leader < 0) {
        /*
         * perf_event_open for CPU cycles failed.  This is a hard error —
         * we cannot measure IPC or MPKI without hardware counters, and
         * synthetic substitutes are forbidden in evaluation builds.
         *
         * Common causes:
         *   /proc/sys/kernel/perf_event_paranoid > 1  (set to <= 1)
         *   Missing CAP_PERFMON capability
         *   Kernel built without CONFIG_PERF_EVENTS
         */
        LOG_ERROR("FATAL: perf_event_open(CYCLES) failed for TID %d (errno=%d: %s). "
                  "Hardware PMU counters are required. "
                  "Fix: echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid",
                  tid, errno, strerror(errno));
        return -1;
    }

    ctx->leader_fd = (int)leader;

    /* 2. Sibling 1: Instructions */
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    pe.disabled = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.read_format = PERF_FORMAT_GROUP;

    long inst = perf_event_open_syscall(&pe, tid, -1, ctx->leader_fd, 0);
    if (inst < 0) {
        LOG_ERROR("FATAL: perf_event_open(INSTRUCTIONS) failed for TID %d (errno=%d: %s). "
                  "Hardware instruction counter is required.",
                  tid, errno, strerror(errno));
        close(ctx->leader_fd);
        ctx->leader_fd = -1;
        return -1;
    }
    ctx->inst_fd = (int)inst;

    /* 3. Sibling 2: LLC Cache Misses */
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = PERF_TYPE_HW_CACHE;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = (PERF_COUNT_HW_CACHE_LL) |
                (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    pe.disabled = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.read_format = PERF_FORMAT_GROUP;

    long llc = perf_event_open_syscall(&pe, tid, -1, ctx->leader_fd, 0);
    if (llc < 0) {
        /* Fallback to generic cache misses */
        pe.type = PERF_TYPE_HARDWARE;
        pe.config = PERF_COUNT_HW_CACHE_MISSES;
        llc = perf_event_open_syscall(&pe, tid, -1, ctx->leader_fd, 0);
    }

    if (llc < 0) {
        LOG_ERROR("FATAL: perf_event_open(LLC_MISSES / CACHE_MISSES) failed for TID %d (errno=%d: %s). "
                  "LLC miss counter is required for MPKI classification.",
                  tid, errno, strerror(errno));
        close(ctx->inst_fd);
        close(ctx->leader_fd);
        ctx->leader_fd = -1;
        ctx->inst_fd = -1;
        return -1;
    }
    ctx->llc_fd = (int)llc;

    /* Reset and enable group leader */
    ioctl(ctx->leader_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    if (ioctl(ctx->leader_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0) {
        LOG_WARN("ioctl ENABLE failed for TID %d (errno=%d)", tid, errno);
    }

    ctx->pmu_active = true;
    ctx->prev_sample.timestamp_ns = get_time_ns();
    return 0;
}

void pmu_close_thread_ctx(thread_pmu_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->leader_fd >= 0) {
        ioctl(ctx->leader_fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        close(ctx->leader_fd);
        ctx->leader_fd = -1;
    }
    if (ctx->inst_fd >= 0) {
        close(ctx->inst_fd);
        ctx->inst_fd = -1;
    }
    if (ctx->llc_fd >= 0) {
        close(ctx->llc_fd);
        ctx->llc_fd = -1;
    }
    ctx->pmu_active = false;
}

int pmu_sample_thread(thread_pmu_ctx_t *ctx, pmu_metrics_t *metrics) {
    if (!ctx || !metrics) return -1;
    memset(metrics, 0, sizeof(*metrics));

    uint64_t now_ns = get_time_ns();
    double dt_sec = (double)(now_ns - ctx->prev_sample.timestamp_ns) / 1000000000.0;
    if (dt_sec <= 0.0) dt_sec = 0.0001;

    metrics->delta_time_sec = dt_sec;
    metrics->current_cpu = pmu_get_thread_cpu(ctx->pid, ctx->tid);

    if (ctx->pmu_active && ctx->leader_fd >= 0) {
        struct perf_group_read_format read_buf;
        memset(&read_buf, 0, sizeof(read_buf));
        ssize_t bytes = read(ctx->leader_fd, &read_buf, sizeof(read_buf));

        if (bytes < (ssize_t)sizeof(read_buf) || read_buf.nr < 3) {
            LOG_DEBUG("Failed to read group counters for TID %d (bytes=%zd, nr=%" PRIu64 ")",
                      ctx->tid, bytes, read_buf.nr);
            return -1;
        }

        uint64_t cur_cycles = read_buf.values[0].value;
        uint64_t cur_inst   = read_buf.values[1].value;
        uint64_t cur_llc    = read_buf.values[2].value;

        uint64_t d_cycles = (cur_cycles >= ctx->prev_sample.cycles) ?
                            (cur_cycles - ctx->prev_sample.cycles) : cur_cycles;
        uint64_t d_inst   = (cur_inst >= ctx->prev_sample.instructions) ?
                            (cur_inst - ctx->prev_sample.instructions) : cur_inst;
        uint64_t d_llc    = (cur_llc >= ctx->prev_sample.llc_misses) ?
                            (cur_llc - ctx->prev_sample.llc_misses) : cur_llc;

        ctx->prev_sample.cycles = cur_cycles;
        ctx->prev_sample.instructions = cur_inst;
        ctx->prev_sample.llc_misses = cur_llc;
        ctx->prev_sample.timestamp_ns = now_ns;

        metrics->delta_cycles = (double)d_cycles;
        metrics->delta_instructions = (double)d_inst;
        metrics->delta_llc_misses = (double)d_llc;

        if (d_cycles > 0) {
            metrics->ipc = (double)d_inst / (double)d_cycles;
        } else {
            metrics->ipc = 0.0;
        }

        if (d_inst > 0) {
            metrics->mpki = ((double)d_llc * 1000.0) / (double)d_inst;
        } else {
            metrics->mpki = 0.0;
        }
    } else {
        /*
         * perf_event_open failed for this thread.  We MUST NOT substitute
         * synthetic or estimated PMU metrics — doing so would silently poison
         * evaluation data destined for a conference paper.
         *
         * Callers must treat a non-zero return as a hard failure.
         * Check /proc/sys/kernel/perf_event_paranoid (must be <= 1 for
         * per-thread hardware counters without CAP_PERFMON) and ensure
         * the kernel is not built with CONFIG_PERF_EVENTS disabled.
         */
        LOG_ERROR("FATAL: perf_event_open unavailable for TID %d — "
                  "hardware PMU counters (IPC, MPKI) are required and cannot be estimated. "
                  "Set perf_event_paranoid <= 1 or run with CAP_PERFMON. "
                  "Aborting measurement.",
                  ctx->tid);
        return -1;
    }

    ctx->last_metrics = *metrics;
    return 0;
}
