#ifndef RAPL_ENERGY_H
#define RAPL_ENERGY_H

#include "common.h"

typedef struct {
    bool available;
    int pkg_fd;
    int core_fd;
    uint64_t max_range_uj;
    uint64_t prev_pkg_uj;
    uint64_t prev_core_uj;
    uint64_t prev_timestamp_ns;

    /* Fallback estimation state */
    double estimated_total_joules;
} rapl_meter_t;

typedef struct {
    double delta_pkg_joules;
    double delta_core_joules;
    double avg_pkg_power_watts;
    double avg_core_power_watts;
    double elapsed_sec;
} rapl_sample_t;

int rapl_init(rapl_meter_t *meter);
void rapl_close(rapl_meter_t *meter);
int rapl_sample(rapl_meter_t *meter, rapl_sample_t *sample);
double rapl_calculate_edp(double joules, double duration_sec);

#endif /* RAPL_ENERGY_H */
