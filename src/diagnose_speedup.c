#include "scheduler_daemon.h"
#include <sys/wait.h>
#include <signal.h>

static double read_sysfs_double(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1.0;
    double v = -1.0;
    if (fscanf(f, "%lf", &v) != 1) v = -1.0;
    fclose(f);
    return v;
}

static double read_cpu_freq_khz(int cpu) {
    if (cpu < 0) return -1.0;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    return read_sysfs_double(path);
}

static int first_cpu_in_mask(const cpu_set_t *mask, int max_cpus) {
    for (int i = 0; i < max_cpus; i++) {
        if (CPU_ISSET(i, mask)) return i;
    }
    return -1;
}

static int last_cpu_in_mask(const cpu_set_t *mask, int max_cpus) {
    for (int i = max_cpus - 1; i >= 0; i--) {
        if (CPU_ISSET(i, mask)) return i;
    }
    return -1;
}

typedef struct {
    int      pinned_cpu;
    int      actual_cpu;
    uint64_t cycles;
    uint64_t instructions;
    double   ipc;
    uint64_t llc_misses;
    double   delta_time_sec;
    double   instr_per_sec;
    double   freq_khz;
    double   uncore_min_khz;
    double   uncore_max_khz;
} diag_sample_t;

static int run_single_sample(const char *const argv_cmd[],
                             int single_cpu,
                             const pmu_hybrid_info_t *hybrid,
                             int warmup_ms,
                             int sample_ms,
                             diag_sample_t *out) {
    memset(out, 0, sizeof(*out));
    out->pinned_cpu = single_cpu;
    out->actual_cpu = -1;

    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(single_cpu, &mask);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) != 0) _exit(127);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv_cmd[0], (char *const *)argv_cmd);
        _exit(127);
    }

    /* Wait for worker pthread if multi-threaded, otherwise use main PID */
    pid_t worker_tid = -1;
    for (int attempt = 0; attempt < 15; attempt++) {
        usleep(20000);
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
    if (worker_tid <= 0) worker_tid = pid;

    thread_pmu_ctx_t ctx;
    if (pmu_init_thread_ctx_ex(&ctx, pid, worker_tid, hybrid) != 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    /* Discard warm-up tick */
    usleep((useconds_t)(warmup_ms * 1000));
    pmu_metrics_t m;
    if (pmu_sample_thread(&ctx, &m) != 0) {
        pmu_close_thread_ctx(&ctx);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    /* Measurement tick */
    usleep((useconds_t)(sample_ms * 1000));
    if (pmu_sample_thread(&ctx, &m) != 0) {
        pmu_close_thread_ctx(&ctx);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    out->actual_cpu     = m.current_cpu;
    out->cycles         = (uint64_t)m.delta_cycles;
    out->instructions   = (uint64_t)m.delta_instructions;
    out->ipc            = m.ipc;
    out->llc_misses     = (uint64_t)m.delta_llc_misses;
    out->delta_time_sec = m.delta_time_sec;
    out->instr_per_sec  = (m.delta_time_sec > 0.0) ? (m.delta_instructions / m.delta_time_sec) : 0.0;
    out->freq_khz       = read_cpu_freq_khz(single_cpu);
    out->uncore_min_khz = read_sysfs_double("/sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/min_freq_khz");
    out->uncore_max_khz = read_sysfs_double("/sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/max_freq_khz");

    pmu_close_thread_ctx(&ctx);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return 0;
}

int main(int argc, char **argv) {
    int trials = 30;
    if (argc > 1) trials = atoi(argv[1]);
    if (trials < 1) trials = 30;

    cpu_topology_t topo;
    if (topology_init(&topo, "config/daemon.conf") != 0) {
        fprintf(stderr, "Failed to initialize topology\n");
        return 1;
    }

    pmu_hybrid_info_t hybrid;
    pmu_detect_hybrid(&hybrid);

    run_provenance_t prov;
    daemon_collect_provenance(&prov);
    cpuset_to_str(&topo.p_core_mask, topo.total_cpus, prov.p_cores_pool, sizeof(prov.p_cores_pool));
    cpuset_to_str(&topo.e_core_mask, topo.total_cpus, prov.e_cores_pool, sizeof(prov.e_cores_pool));
    snprintf(prov.excluded_siblings, sizeof(prov.excluded_siblings), "18-25");

    int cpu_p = first_cpu_in_mask(&topo.p_core_mask, topo.total_cpus);
    int cpu_e = first_cpu_in_mask(&topo.e_core_mask, topo.total_cpus);
    int cpu_harness = last_cpu_in_mask(&topo.e_core_mask, topo.total_cpus);
    if (cpu_p < 0 || cpu_e < 0 || cpu_harness < 0 || cpu_harness == cpu_e) {
        fprintf(stderr, "Invalid pool CPUs: P=%d E=%d harness=%d\n", cpu_p, cpu_e, cpu_harness);
        return 1;
    }

    /* Pin this diagnostic driver away from both measured cores and excluded siblings */
    cpu_set_t self_mask;
    CPU_ZERO(&self_mask);
    CPU_SET(cpu_harness, &self_mask);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &self_mask) != 0) {
        perror("sched_setaffinity(self)");
        return 1;
    }

    printf("Pinned diagnostic driver to CPU %d | P-core=%d | E-core=%d | trials=%d\n",
           cpu_harness, cpu_p, cpu_e, trials);

    const char *comp_cmd[] = {"./bin/bench_compute", "--iterations", "0", "--duration", "3.0", "--matrix-size", "128", NULL};
    const char *mem_cmd[]  = {"./bin/bench_memory",  "--iterations", "0", "--duration", "3.0", "--buffer-mb", "64", NULL};
    const char *dram_cmd[] = {"./bin/bench_dram_chase", "--duration", "3.0", "--buffer-mb", "256", NULL};

    struct {
        const char *name;
        const char *const *cmd;
        int warmup_ms;
        int sample_ms;
    } benches[] = {
        {"bench_compute",    comp_cmd, 150, 200},
        {"bench_memory",     mem_cmd,  350, 200},
        {"bench_dram_chase", dram_cmd, 350, 200},
    };

    FILE *csv = fopen("results/diagnose_speedup_samples.csv", "w");
    if (!csv) {
        perror("fopen results/diagnose_speedup_samples.csv");
        return 1;
    }
    fprintf(csv,
            "git_commit,kernel_version,cpu_model,governor,turbo_state,"
            "rapl_source,p_cores,e_cores,excluded_siblings,trial_index,timestamp_iso,"
            "benchmark,order,pool,pinned_cpu,actual_cpu,cycles,instructions,"
            "ipc,llc_misses,delta_time_sec,instr_per_sec,freq_khz,"
            "uncore_min_khz,uncore_max_khz\n");

    srand(20261125);
    for (int b = 0; b < 3; b++) {
        printf("Running %d randomized P/E trials for %s...\n", trials, benches[b].name);
        fflush(stdout);
        for (int t = 1; t <= trials; t++) {
            int p_first = rand() % 2;
            const char *order_str = p_first ? "P_then_E" : "E_then_P";
            for (int step = 0; step < 2; step++) {
                int is_p = p_first ? (step == 0) : (step == 1);
                int target_cpu = is_p ? cpu_p : cpu_e;
                const char *pool_str = is_p ? "P-Core" : "E-Core";

                diag_sample_t s;
                if (run_single_sample(benches[b].cmd, target_cpu, &hybrid,
                                      benches[b].warmup_ms, benches[b].sample_ms, &s) != 0) {
                    fprintf(stderr, "[ERROR] Failed sample: bench=%s trial=%d pool=%s\n",
                            benches[b].name, t, pool_str);
                    fclose(csv);
                    return 1;
                }

                time_t wall = time(NULL);
                char iso[32];
                strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", gmtime(&wall));

                fprintf(csv,
                        "%s,%s,\"%s\",%s,%s,"
                        "%s,\"%s\",\"%s\",\"%s\",%d,%s,"
                        "%s,%s,%s,%d,%d,%" PRIu64 ",%" PRIu64 ","
                        "%.4f,%" PRIu64 ",%.6f,%.1f,%.1f,%.1f,%.1f\n",
                        prov.git_commit, prov.kernel_version, prov.cpu_model,
                        prov.governor, prov.turbo_state, prov.rapl_source,
                        prov.p_cores_pool, prov.e_cores_pool, prov.excluded_siblings,
                        t, iso,
                        benches[b].name, order_str, pool_str,
                        s.pinned_cpu, s.actual_cpu, s.cycles, s.instructions,
                        s.ipc, s.llc_misses, s.delta_time_sec, s.instr_per_sec,
                        s.freq_khz, s.uncore_min_khz, s.uncore_max_khz);
            }
        }
    }

    fclose(csv);
    printf("Saved raw samples to results/diagnose_speedup_samples.csv\n");
    return 0;
}
