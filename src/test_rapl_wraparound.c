#include "rapl_energy.h"
#include <math.h>

#define TEST_ASSERT(cond)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "ASSERTION FAILED (%s:%d): %s\n",                   \
                    __FILE__, __LINE__, #cond);                                 \
            return 1;                                                           \
        }                                                                       \
    } while (0)

int main(void) {
    uint64_t delta = 0;

    /* 1. Normal monotonic increment */
    TEST_ASSERT(rapl_compute_delta_uj(1000000ULL, 2500000ULL, 262143328850ULL, &delta) == 0);
    TEST_ASSERT(delta == 1500000ULL);

    /* 2. Zero delta (identical readings) */
    TEST_ASSERT(rapl_compute_delta_uj(500000ULL, 500000ULL, 262143328850ULL, &delta) == 0);
    TEST_ASSERT(delta == 0ULL);

    /* 3. Counter wraparound across max_range_uj */
    uint64_t max_r = 262143328850ULL;
    uint64_t prev = max_r - 1000000ULL;
    uint64_t cur  = 500000ULL;
    TEST_ASSERT(rapl_compute_delta_uj(prev, cur, max_r, &delta) == 0);
    TEST_ASSERT(delta == 1500000ULL);

    /* 4. Exact boundary wraparound (cur == 0, prev == max_r - 1) */
    TEST_ASSERT(rapl_compute_delta_uj(max_r - 1ULL, 0ULL, max_r, &delta) == 0);
    TEST_ASSERT(delta == 1ULL);

    /* 5. Wraparound when max_range_uj == 0 must fail loudly (-1) */
    TEST_ASSERT(rapl_compute_delta_uj(1000ULL, 500ULL, 0ULL, &delta) == -1);
    TEST_ASSERT(delta == 0ULL);

    /* 6. Null pointer guard */
    TEST_ASSERT(rapl_compute_delta_uj(0ULL, 10ULL, max_r, NULL) == -1);

    /* 7. EDP calculation */
    TEST_ASSERT(fabs(rapl_calculate_edp(10.0, 2.5) - 25.0) < 1e-9);
    TEST_ASSERT(fabs(rapl_calculate_edp(-5.0, 2.0) - 0.0) < 1e-9);
    TEST_ASSERT(fabs(rapl_calculate_edp(5.0, -2.0) - 0.0) < 1e-9);

    printf("[PASS] All RAPL wraparound and EDP unit tests passed!\n");
    return 0;
}
