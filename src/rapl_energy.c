#include "rapl_energy.h"

#define RAPL_PKG_PATH "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj"
#define RAPL_CORE_PATH "/sys/class/powercap/intel-rapl/intel-rapl:0:0/energy_uj"
#define RAPL_MAX_RANGE_PATH "/sys/class/powercap/intel-rapl/intel-rapl:0/max_energy_range_uj"

static uint64_t read_uj_from_fd(int fd) {
    if (fd < 0) return 0;
    char buf[64];
    if (lseek(fd, 0, SEEK_SET) < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (uint64_t)strtoull(buf, NULL, 10);
}

int rapl_init(rapl_meter_t *meter) {
    if (!meter) return -1;
    memset(meter, 0, sizeof(*meter));
    meter->pkg_fd = -1;
    meter->core_fd = -1;
    meter->max_range_uj = 0;

    int max_fd = open(RAPL_MAX_RANGE_PATH, O_RDONLY);
    if (max_fd >= 0) {
        char buf[64];
        ssize_t n = read(max_fd, buf, sizeof(buf) - 1);
        close(max_fd);
        if (n > 0) {
            buf[n] = '\0';
            meter->max_range_uj = (uint64_t)strtoull(buf, NULL, 10);
        }
    }
    if (meter->max_range_uj == 0) {
        meter->max_range_uj = 262143328850ULL; /* Default ~262k Joules */
    }

    meter->pkg_fd = open(RAPL_PKG_PATH, O_RDONLY);
    if (meter->pkg_fd < 0) {
        LOG_INFO("Intel RAPL package sysfs (%s) not accessible (errno=%d: %s). Activating energy model fallback.",
                 RAPL_PKG_PATH, errno, strerror(errno));
        meter->available = false;
        meter->prev_timestamp_ns = get_time_ns();
        return 0;
    }

    meter->core_fd = open(RAPL_CORE_PATH, O_RDONLY);
    meter->prev_pkg_uj = read_uj_from_fd(meter->pkg_fd);
    meter->prev_core_uj = (meter->core_fd >= 0) ? read_uj_from_fd(meter->core_fd) : 0;
    meter->prev_timestamp_ns = get_time_ns();
    meter->available = true;

    LOG_INFO("Intel RAPL telemetry initialized successfully (Package Initial = %" PRIu64 " uJ).",
             meter->prev_pkg_uj);
    return 0;
}

void rapl_close(rapl_meter_t *meter) {
    if (!meter) return;
    if (meter->pkg_fd >= 0) {
        close(meter->pkg_fd);
        meter->pkg_fd = -1;
    }
    if (meter->core_fd >= 0) {
        close(meter->core_fd);
        meter->core_fd = -1;
    }
    meter->available = false;
}

int rapl_sample(rapl_meter_t *meter, rapl_sample_t *sample) {
    if (!meter || !sample) return -1;
    memset(sample, 0, sizeof(*sample));

    uint64_t now_ns = get_time_ns();
    double dt_sec = (double)(now_ns - meter->prev_timestamp_ns) / 1000000000.0;
    if (dt_sec <= 0.0) dt_sec = 0.0001;
    sample->elapsed_sec = dt_sec;

    if (meter->available && meter->pkg_fd >= 0) {
        uint64_t cur_pkg = read_uj_from_fd(meter->pkg_fd);
        uint64_t cur_core = (meter->core_fd >= 0) ? read_uj_from_fd(meter->core_fd) : 0;

        uint64_t d_pkg_uj = (cur_pkg >= meter->prev_pkg_uj) ?
            (cur_pkg - meter->prev_pkg_uj) :
            (cur_pkg + meter->max_range_uj - meter->prev_pkg_uj);

        uint64_t d_core_uj = (meter->core_fd >= 0) ? (
            (cur_core >= meter->prev_core_uj) ?
            (cur_core - meter->prev_core_uj) :
            (cur_core + meter->max_range_uj - meter->prev_core_uj)
        ) : 0;

        meter->prev_pkg_uj = cur_pkg;
        meter->prev_core_uj = cur_core;
        meter->prev_timestamp_ns = now_ns;

        sample->delta_pkg_joules = (double)d_pkg_uj / 1000000.0;
        sample->delta_core_joules = (double)d_core_uj / 1000000.0;
        sample->avg_pkg_power_watts = sample->delta_pkg_joules / dt_sec;
        sample->avg_core_power_watts = sample->delta_core_joules / dt_sec;
    } else {
        /* Fallback power model: ~45W baseline package power */
        meter->prev_timestamp_ns = now_ns;
        double default_watts = 45.0;
        sample->avg_pkg_power_watts = default_watts;
        sample->delta_pkg_joules = default_watts * dt_sec;
        sample->avg_core_power_watts = default_watts * 0.7;
        sample->delta_core_joules = sample->avg_core_power_watts * dt_sec;
        meter->estimated_total_joules += sample->delta_pkg_joules;
    }

    return 0;
}

double rapl_calculate_edp(double joules, double duration_sec) {
    if (joules < 0.0) joules = 0.0;
    if (duration_sec < 0.0) duration_sec = 0.0;
    return joules * duration_sec;
}
