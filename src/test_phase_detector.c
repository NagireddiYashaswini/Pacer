#include "phase_detector.h"

#define TEST_ASSERT(cond)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "ASSERTION FAILED (%s:%d): %s\n",                   \
                    __FILE__, __LINE__, #cond);                                 \
            return 1;                                                           \
        }                                                                       \
    } while (0)

int main(void) {
    LOG_INFO("Running Sliding-Window Phase-Change Detector Unit Tests...");

    phase_detector_t pd;
    phase_detector_init(&pd, 5, 0.08, 0.25);

    phase_stat_t stats;

    /* 1. Feed 5 compute-bound samples (IPC ~ 2.5, MPKI ~ 0.2) */
    for (int i = 0; i < 5; i++) {
        bool shift = phase_detector_push(&pd, 2.5 + ((i % 2) ? 0.05 : -0.05), 0.2, &stats);
        if (i >= 2) {
            TEST_ASSERT(shift == false);
        }
        TEST_ASSERT(stats.is_compute_bound == true);
        TEST_ASSERT(stats.is_memory_bound == false);
    }
    TEST_ASSERT(fabs(stats.rolling_mean - 2.49) < 0.05);
    TEST_ASSERT(stats.rolling_variance < 0.01);

    /* 2. Inject sudden algorithmic phase shift to memory-bound (IPC = 0.35, MPKI = 25.0) */
    bool shift_detected = false;
    for (int i = 0; i < 5; i++) {
        bool shift = phase_detector_push(&pd, 0.35 + ((i % 2) ? 0.02 : -0.02), 25.0, &stats);
        if (shift) shift_detected = true;
    }

    TEST_ASSERT(shift_detected == true);
    TEST_ASSERT(stats.is_memory_bound == true);
    TEST_ASSERT(stats.is_compute_bound == false);
    /* After 5 samples in window of size 5, steady-state variance should settle */
    TEST_ASSERT(stats.is_phase_shifting == false);
    TEST_ASSERT(fabs(stats.rolling_mean - 0.35) < 0.05);

    /* 3. Reset should clear history */
    phase_detector_reset(&pd);
    TEST_ASSERT(pd.count == 0);
    TEST_ASSERT(pd.head == 0);

    /* 4. Boundary classification: moderate MPKI (12.0) with low IPC (0.7) -> memory bound */
    phase_detector_push(&pd, 0.7, 12.0, &stats);
    TEST_ASSERT(stats.is_memory_bound == true);
    TEST_ASSERT(stats.is_compute_bound == false);

    LOG_INFO("Phase Detector Unit Tests PASSED!");
    return 0;
}
