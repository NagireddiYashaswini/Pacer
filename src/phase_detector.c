#include "phase_detector.h"

void phase_detector_init(phase_detector_t *pd, int window_size,
                         double var_threshold, double mean_shift_threshold) {
    if (!pd) return;
    memset(pd, 0, sizeof(*pd));
    if (window_size <= 2) window_size = 5;
    if (window_size > MAX_WINDOW_SIZE) window_size = MAX_WINDOW_SIZE;

    pd->window_size = window_size;
    pd->variance_threshold = (var_threshold > 0.0) ? var_threshold : 0.08;
    pd->mean_shift_threshold = (mean_shift_threshold > 0.0) ? mean_shift_threshold : 0.25;
    pd->count = 0;
    pd->head = 0;
}

void phase_detector_reset(phase_detector_t *pd) {
    if (!pd) return;
    pd->count = 0;
    pd->head = 0;
    memset(&pd->last_stats, 0, sizeof(pd->last_stats));
}

bool phase_detector_push(phase_detector_t *pd, double ipc, double mpki, phase_stat_t *stats_out) {
    if (!pd) return false;

    /* Push sample into circular buffer */
    pd->samples[pd->head] = ipc;
    pd->mpki_samples[pd->head] = mpki;
    pd->head = (pd->head + 1) % pd->window_size;
    if (pd->count < pd->window_size) {
        pd->count++;
    }

    int N = pd->count;
    double sum = 0.0;
    for (int i = 0; i < N; i++) {
        sum += pd->samples[i];
    }
    double mean = sum / (double)N;

    double var = 0.0;
    if (N > 1) {
        double sq_sum = 0.0;
        for (int i = 0; i < N; i++) {
            double diff = pd->samples[i] - mean;
            sq_sum += diff * diff;
        }
        var = sq_sum / (double)(N - 1);
    }
    double stddev = sqrt(var);

    phase_stat_t stats;
    stats.rolling_mean = mean;
    stats.rolling_variance = var;
    stats.rolling_stddev = stddev;
    stats.current_ipc = ipc;
    stats.current_mpki = mpki;

    /* Classify workload nature */
    stats.is_memory_bound = (mpki >= 15.0 || (mpki >= 10.0 && ipc < 0.8));
    stats.is_compute_bound = (mpki < 5.0 && ipc >= 1.2);

    /* Phase change detection:
       1. Normalized variance spike: var / (mean^2 + 0.01) > threshold
       2. Immediate deviation of current sample from history mean: |ipc - mean| / (mean + 0.01) > threshold
       3. Sudden transition crossing the MPKI memory barrier */
    bool variance_spike = false;
    if (N >= 3) {
        double norm_var = var / (mean * mean + 0.001);
        if (norm_var > pd->variance_threshold) {
            variance_spike = true;
        }
    }

    bool mean_shift = false;
    if (N >= 3 && mean > 0.01) {
        double rel_diff = fabs(ipc - mean) / mean;
        if (rel_diff > pd->mean_shift_threshold) {
            mean_shift = true;
        }
    }

    stats.is_phase_shifting = (variance_spike || mean_shift);

    pd->last_stats = stats;
    if (stats_out) {
        *stats_out = stats;
    }

    return stats.is_phase_shifting;
}
