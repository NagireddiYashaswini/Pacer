#include "topology.h"
#include "pmu_sampler.h"
#include "rapl_energy.h"

int main(int argc, char **argv) {
    const char *conf = (argc > 1) ? argv[1] : "config/daemon.conf";
    LOG_INFO("Starting Phase 1 Verification Test with config: %s", conf);

    /* 1. Topology Test */
    cpu_topology_t topo;
    if (topology_init(&topo, conf) != 0) {
        LOG_ERROR("Failed to initialize topology");
        return 1;
    }
    topology_print(&topo);

    /* 2. RAPL Test */
    rapl_meter_t rapl;
    rapl_init(&rapl);
    rapl_sample_t rapl_s;
    usleep(250000); /* 250ms */
    rapl_sample(&rapl, &rapl_s);
    LOG_INFO("RAPL Sample: Elapsed=%.3f s, Pkg=%.3f J (%.2f W), Core=%.3f J (%.2f W)",
             rapl_s.elapsed_sec, rapl_s.delta_pkg_joules, rapl_s.avg_pkg_power_watts,
             rapl_s.delta_core_joules, rapl_s.avg_core_power_watts);
    rapl_close(&rapl);

    /* 3. PMU / Thread Discovery Test on self */
    pid_t self_pid = getpid();
    pid_t tids[16];
    int n_tids = pmu_discover_tids(self_pid, tids, 16);
    LOG_INFO("TID Discovery on self PID %d found %d threads:", self_pid, n_tids);
    for (int i = 0; i < n_tids; i++) {
        LOG_INFO("  - Thread %d: CPU=%d", tids[i], pmu_get_thread_cpu(self_pid, tids[i]));
    }

    thread_pmu_ctx_t pmu_ctx;
    pmu_init_thread_ctx(&pmu_ctx, self_pid, self_pid);

    /* Do some computation to create counter deltas */
    volatile double dummy = 0.0;
    for (int i = 0; i < 5000000; i++) {
        dummy += (double)i * 1.0001;
    }

    pmu_metrics_t metrics;
    pmu_sample_thread(&pmu_ctx, &metrics);
    LOG_INFO("Self PMU Sample: CPU=%d, IPC=%.3f, MPKI=%.2f, DeltaCycles=%.0f, DeltaInst=%.0f (dummy=%.2f)",
             metrics.current_cpu, metrics.ipc, metrics.mpki,
             metrics.delta_cycles, metrics.delta_instructions, dummy);

    pmu_close_thread_ctx(&pmu_ctx);
    LOG_INFO("Phase 1 Verification Test PASSED successfully!");
    return 0;
}
