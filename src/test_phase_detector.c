#include "phase_detector.h"
#include <assert.h>

int main(void) {
    LOG_INFO("Running Sliding-Window Phase-Change Detector Unit Tests...");

    phase_detector_t pd;
    phase_detector_init(&pd, 5, 0.08, 0.25);

    phase_stat_t stats;

    /* Feed 5 compute-bound samples (IPC ~ 2.5, MPKI ~ 0.2) */
    LOG_INFO("Feeding Stable Compute Phase (IPC=2.5, MPKI=0.2)...");
    for (int i = 0; i < 5; i++) {
        bool shift = phase_detector_push(&pd, 2.5 + ((i % 2) ? 0.05 : -0.05), 0.2, &stats);
        LOG_INFO("Tick %d: IPC=%.2f, Mean=%.2f, Var=%.4f, Shift=%s, Compute=%s, Memory=%s",
                 i + 1, stats.current_ipc, stats.rolling_mean, stats.rolling_variance,
                 shift ? "YES" : "NO",
                 stats.is_compute_bound ? "YES" : "NO",
                 stats.is_memory_bound ? "YES" : "NO");
    }
    assert(stats.is_compute_bound == true);
    assert(stats.is_memory_bound == false);

    /* Now inject a sudden algorithmic phase shift to memory-bound (IPC = 0.35, MPKI = 25.0) */
    LOG_INFO("\nInjecting Sudden Algorithmic Phase Shift to Memory-Bound (IPC=0.35, MPKI=25.0)...");
    bool shift_detected = false;
    for (int i = 0; i < 5; i++) {
        bool shift = phase_detector_push(&pd, 0.35 + ((i % 2) ? 0.02 : -0.02), 25.0, &stats);
        if (shift) shift_detected = true;
        LOG_INFO("Tick %d: IPC=%.2f, Mean=%.2f, Var=%.4f, Shift=%s, Compute=%s, Memory=%s",
                 i + 6, stats.current_ipc, stats.rolling_mean, stats.rolling_variance,
                 shift ? "YES" : "NO",
                 stats.is_compute_bound ? "YES" : "NO",
                 stats.is_memory_bound ? "YES" : "NO");
    }

    assert(shift_detected == true);
    assert(stats.is_memory_bound == true);
    assert(stats.is_compute_bound == false);

    LOG_INFO("\nPhase Detector Unit Test PASSED with flying colors!");
    return 0;
}
