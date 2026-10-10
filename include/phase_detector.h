#ifndef PHASE_DETECTOR_H
#define PHASE_DETECTOR_H

#include "common.h"
#include <math.h>

#define MAX_WINDOW_SIZE 64

typedef struct {
    double rolling_mean;
    double rolling_variance;
    double rolling_stddev;
    double current_ipc;
    double current_mpki;
    bool is_phase_shifting;
    bool is_memory_bound;
    bool is_compute_bound;
} phase_stat_t;

typedef struct {
    int window_size;
    int count;
    int head;
    double samples[MAX_WINDOW_SIZE];
    double mpki_samples[MAX_WINDOW_SIZE];
    double variance_threshold;
    double mean_shift_threshold;
    phase_stat_t last_stats;
} phase_detector_t;

void phase_detector_init(phase_detector_t *pd, int window_size,
                         double var_threshold, double mean_shift_threshold);

/* Pushes new IPC & MPKI sample into sliding window.
   Returns true if an algorithmic phase shift was detected on this tick. */
bool phase_detector_push(phase_detector_t *pd, double ipc, double mpki, phase_stat_t *stats_out);

void phase_detector_reset(phase_detector_t *pd);

#endif /* PHASE_DETECTOR_H */
