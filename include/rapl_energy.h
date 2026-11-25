#ifndef RAPL_ENERGY_H
#define RAPL_ENERGY_H

#include "common.h"
#include <dirent.h>

typedef struct {
    bool     available;
    int      pkg_fd;
    int      core_fd;
    uint64_t max_range_uj;     /* For wraparound correction; 0 = unknown */
    uint64_t prev_pkg_uj;
    uint64_t prev_core_uj;
    uint64_t prev_timestamp_ns;

    /* Resolved sysfs paths (populated by rapl_init via powercap scan) */
    char pkg_path[MAX_PATH_LEN];
    char core_path[MAX_PATH_LEN];
} rapl_meter_t;

typedef struct {
    double delta_pkg_joules;
    double delta_core_joules;
    double avg_pkg_power_watts;
    double avg_core_power_watts;
    double elapsed_sec;
} rapl_sample_t;

/*
 * Scan /sys/class/powercap/intel-rapl/ to find the package domain and
 * optional core/PP0 sub-domain.  Opens fds and reads max_energy_range_uj.
 * Returns  0 on success (meter->available = true).
 * Returns -1 on failure — energy measurement cannot proceed.
 *           A detailed error has been printed to stderr.
 */
int rapl_init(rapl_meter_t *meter);

void rapl_close(rapl_meter_t *meter);

/*
 * Read current counter, compute delta with proper wraparound correction.
 * Returns 0 on success, -1 if RAPL is unavailable (programmer error).
 */
int rapl_sample(rapl_meter_t *meter, rapl_sample_t *sample);

/*
 * Compute microjoule delta between prev_uj and cur_uj, handling counter
 * wraparound at max_range_uj.
 * Returns 0 on success (writing *delta_out), -1 if cur_uj < prev_uj and
 * max_range_uj == 0 (cannot correct wraparound).
 */
int rapl_compute_delta_uj(uint64_t prev_uj, uint64_t cur_uj,
                          uint64_t max_range_uj, uint64_t *delta_out);

double rapl_calculate_edp(double joules, double duration_sec);

#endif /* RAPL_ENERGY_H */
