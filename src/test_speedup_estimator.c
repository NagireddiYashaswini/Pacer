#include "scheduler_daemon.h"
#include <sys/wait.h>
#include <signal.h>

static double read_cpu_cur_freq_khz(int cpu) {
    if (cpu < 0) return 0.0;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    FILE *f = fopen(path, "r");
    if (!f) return 0.0;
    double khz = 0.0;
    if (fscanf(f, "%lf", &khz) != 1) khz = 0.0;
    fclose(f);
    return khz;
}

typedef struct {
    double ips;
    double avg_freq_khz;
    int    samples;
} pool_meas_t;

/*
 * Spawn `argv_cmd` pinned to `pool_mask`, discard `discard_ticks` warm-up ticks,
 * then measure `meas_ticks` ticks using the exact same PMU sampler and
 * throughput calculation (delta_instructions / delta_time_sec) as the daemon.
 */
static int measure_bench_on_pool(const char *const argv_cmd[],
                                 const cpu_set_t *pool_mask,
                                 const pmu_hybrid_info_t *hybrid,
                                 int discard_ticks,
                                 int meas_ticks,
                                 int interval_ms,
                                 pool_meas_t *out) {
    memset(out, 0, sizeof(*out));

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        if (sched_setaffinity(0, sizeof(cpu_set_t), pool_mask) != 0) {
            perror("sched_setaffinity");
            _exit(127);
        }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv_cmd[0], (char *const *)argv_cmd);
        perror("execvp");
        _exit(127);
    }

    /* Wait up to 1s for the benchmark to spawn its worker pthread */
    pid_t worker_tid = -1;
    for (int attempt = 0; attempt < 40; attempt++) {
        usleep(25000);
        pid_t tids[16];
        int n = pmu_discover_tids(pid, tids, 16);
        for (int i = 0; i < n; i++) {
            if (tids[i] != pid) {
                worker_tid = tids[i];
                break;
            }
        }
        if (worker_tid > 0) break;
    }
    if (worker_tid <= 0) {
        worker_tid = pid; /* Single-threaded fallback */
    }

    /* Let worker settle into its kernel loop */
    usleep(100000);

    thread_pmu_ctx_t ctx;
    if (pmu_init_thread_ctx_ex(&ctx, pid, worker_tid, hybrid) != 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    double total_inst = 0.0;
    double total_sec  = 0.0;
    double freq_sum   = 0.0;
    int    freq_cnt   = 0;
    int    valid_samples = 0;

    int total_ticks = discard_ticks + meas_ticks;
    for (int t = 0; t < total_ticks; t++) {
        usleep((useconds_t)(interval_ms * 1000));
        pmu_metrics_t m;
        if (pmu_sample_thread(&ctx, &m) != 0) {
            break;
        }
        if (t < discard_ticks) {
            continue; /* Discard warm-up tick */
        }
        total_inst += m.delta_instructions;
        total_sec  += m.delta_time_sec;
        double f = read_cpu_cur_freq_khz(m.current_cpu);
        if (f > 0.0) {
            freq_sum += f;
            freq_cnt++;
        }
        valid_samples++;
    }

    pmu_close_thread_ctx(&ctx);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    if (valid_samples == 0 || total_sec <= 0.0) {
        return -1;
    }

    out->ips = total_inst / total_sec;
    out->avg_freq_khz = (freq_cnt > 0) ? (freq_sum / freq_cnt) : 0.0;
    out->samples = valid_samples;
    return 0;
}

int main(void) {
    cpu_topology_t topo;
    if (topology_init(&topo, "config/daemon.conf") != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize topology from config/pools.yaml\n");
        return 1;
    }

    pmu_hybrid_info_t hybrid;
    pmu_detect_hybrid(&hybrid);

    const char *compute_cmd[] = {
        "./bin/bench_compute", "--iterations", "0", "--duration", "4.0", "--matrix-size", "128", NULL
    };
    const char *memory_cmd[] = {
        "./bin/bench_memory", "--iterations", "0", "--duration", "4.0", "--buffer-mb", "64", NULL
    };

    const int discard_ticks = 1;
    const int meas_ticks    = 5;
    const int interval_ms   = 150;

    pool_meas_t comp_p, comp_e, mem_p, mem_e;

    if (measure_bench_on_pool(compute_cmd, &topo.p_core_mask, &hybrid,
                              discard_ticks, meas_ticks, interval_ms, &comp_p) != 0 ||
        measure_bench_on_pool(compute_cmd, &topo.e_core_mask, &hybrid,
                              discard_ticks, meas_ticks, interval_ms, &comp_e) != 0) {
        fprintf(stderr, "[ERROR] Failed to measure bench_compute on P/E pools\n");
        return 1;
    }

    if (measure_bench_on_pool(memory_cmd, &topo.p_core_mask, &hybrid,
                              discard_ticks, meas_ticks, interval_ms, &mem_p) != 0 ||
        measure_bench_on_pool(memory_cmd, &topo.e_core_mask, &hybrid,
                              discard_ticks, meas_ticks, interval_ms, &mem_e) != 0) {
        fprintf(stderr, "[ERROR] Failed to measure bench_memory on P/E pools\n");
        return 1;
    }

    double speedup_compute = scheduler_compute_speedup(comp_p.ips, comp_e.ips);
    double speedup_memory  = scheduler_compute_speedup(mem_p.ips, mem_e.ips);

    double avg_p_freq = (comp_p.avg_freq_khz + mem_p.avg_freq_khz) / 2.0;
    double avg_e_freq = (comp_e.avg_freq_khz + mem_e.avg_freq_khz) / 2.0;
    double freq_ratio = (avg_e_freq > 0.0) ? (avg_p_freq / avg_e_freq) : 0.0;

    printf("=======================================================================\n");
    printf("  SPEEDUP ESTIMATOR DIRECTIONAL TEST (real PMU + real P/E pools)\n");
    printf("=======================================================================\n");
    printf("bench_compute:\n");
    printf("  P-pool: %.1f instr/s (freq=%.0f kHz, samples=%d)\n",
           comp_p.ips, comp_p.avg_freq_khz, comp_p.samples);
    printf("  E-pool: %.1f instr/s (freq=%.0f kHz, samples=%d)\n",
           comp_e.ips, comp_e.avg_freq_khz, comp_e.samples);
    printf("  Measured Speedup (P/E): %.4fx\n", speedup_compute);
    printf("bench_memory:\n");
    printf("  P-pool: %.1f instr/s (freq=%.0f kHz, samples=%d)\n",
           mem_p.ips, mem_p.avg_freq_khz, mem_p.samples);
    printf("  E-pool: %.1f instr/s (freq=%.0f kHz, samples=%d)\n",
           mem_e.ips, mem_e.avg_freq_khz, mem_e.samples);
    printf("  Measured Speedup (P/E): %.4fx\n", speedup_memory);
    printf("Measured P/E Frequency Ratio: %.4fx (P=%.0f kHz, E=%.0f kHz)\n",
           freq_ratio, avg_p_freq, avg_e_freq);
    printf("=======================================================================\n");

    if (speedup_compute > speedup_memory) {
        printf("[PASS] speedup_compute (%.4fx) > speedup_memory (%.4fx)\n",
               speedup_compute, speedup_memory);
        return 0;
    } else {
        fprintf(stderr,
                "[WARN] Machine did not show speedup_compute (%.4fx) > speedup_memory (%.4fx).\n",
                speedup_compute, speedup_memory);
        return 1;
    }
}
