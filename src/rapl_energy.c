#include "rapl_energy.h"

/*
 * RAPL sysfs path resolution.
 *
 * The canonical location is under /sys/devices/virtual/powercap/intel-rapl/,
 * which is also reachable via the /sys/class/powercap/intel-rapl/ symlink.
 * The harness previously used the WRONG path /sys/class/powercap/intel-rapl:0/
 * (missing the "intel-rapl/" prefix directory).
 *
 * We scan /sys/class/powercap/ once to find the correct package domain,
 * identified by its "name" file containing "package".  This handles systems
 * with non-standard numbering and avoids hardcoded paths.
 *
 * Sub-domain "intel-rapl:0:0" is the core (PP0) domain when present.
 */

/* Silence path-length truncation analysis (paths are always < MAX_PATH_LEN) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"

#define POWERCAP_ROOT   "/sys/class/powercap"
#define RAPL_PREFIX     "intel-rapl:"

/*
 * Scan POWERCAP_ROOT/intel-rapl/ for a domain whose "name" file contains
 * "package".  Write the energy_uj path into out_path (len bytes).
 * Returns 0 on success, -1 if not found.
 */
static int rapl_find_domain_path(const char *domain_name_substr,
                                  char *out_path, size_t len) {
    /* Base directory where RAPL domains live */
    const char *rapl_dir = POWERCAP_ROOT "/intel-rapl";

    DIR *dir = opendir(rapl_dir);
    if (!dir) {
        /* Try the alternative sysfs location */
        rapl_dir = "/sys/devices/virtual/powercap/intel-rapl";
        dir = opendir(rapl_dir);
        if (!dir) return -1;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        /* Only look at entries that start with "intel-rapl:" */
        if (strncmp(de->d_name, RAPL_PREFIX, strlen(RAPL_PREFIX)) != 0)
            continue;

        char name_path[MAX_PATH_LEN];
        snprintf(name_path, sizeof(name_path), "%s/%s/name", rapl_dir, de->d_name);
        FILE *f = fopen(name_path, "r");
        if (!f) continue;
        char name_buf[64] = {0};
        if (fgets(name_buf, sizeof(name_buf), f)) {
            /* Strip trailing whitespace/newline */
            size_t l = strlen(name_buf);
            while (l > 0 && (name_buf[l-1] == '\n' || name_buf[l-1] == '\r'
                              || name_buf[l-1] == ' ')) {
                name_buf[--l] = '\0';
            }
        }
        fclose(f);

        if (strstr(name_buf, domain_name_substr) != NULL) {
            snprintf(out_path, len, "%s/%s/energy_uj", rapl_dir, de->d_name);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

/*
 * Find the sub-domain (PP0 / core) path under the given package directory.
 * The sub-domain entry is a directory inside the package directory whose
 * own "name" file contains "core" or is otherwise a sub-domain of pkg.
 */
static int rapl_find_subdomain_path(const char *pkg_domain_dir,
                                     char *out_path, size_t len) {
    DIR *dir = opendir(pkg_domain_dir);
    if (!dir) return -1;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, RAPL_PREFIX, strlen(RAPL_PREFIX)) != 0)
            continue;

        char name_path[MAX_PATH_LEN];
        snprintf(name_path, sizeof(name_path), "%s/%s/name",
                 pkg_domain_dir, de->d_name);
        FILE *f = fopen(name_path, "r");
        if (!f) continue;
        char name_buf[64] = {0};
        bool got_name = (fgets(name_buf, sizeof(name_buf), f) != NULL);
        fclose(f);
        if (!got_name) continue;

        /* Accept "core" or any non-empty name as a valid sub-domain */
        size_t l = strlen(name_buf);
        while (l > 0 && (name_buf[l-1] == '\n' || name_buf[l-1] == '\r'
                          || name_buf[l-1] == ' ')) name_buf[--l] = '\0';

        if (l > 0) {
            char energy_path[MAX_PATH_LEN];
            snprintf(energy_path, sizeof(energy_path),
                     "%s/%s/energy_uj", pkg_domain_dir, de->d_name);
            /* Verify the energy_uj file actually exists and is readable */
            if (access(energy_path, R_OK) == 0) {
                snprintf(out_path, len, "%s", energy_path);
                out_path[len - 1] = '\0';
                closedir(dir);
                return 0;
            }
        }
    }
    closedir(dir);
    return -1;
}

static uint64_t read_uj_from_fd(int fd) {
    if (fd < 0) return 0;
    char buf[64];
    if (lseek(fd, 0, SEEK_SET) < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (uint64_t)strtoull(buf, NULL, 10);
}

static uint64_t read_uj_from_path(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), f) == NULL) buf[0] = '0';
    fclose(f);
    return (uint64_t)strtoull(buf, NULL, 10);
}

/* -------------------------------------------------------------------------
 * rapl_init: scan powercap sysfs, open fds, read max_range_uj.
 *
 * Returns  0 on success (meter->available = true).
 * Returns -1 on failure — energy measurement cannot proceed.
 *          The caller MUST NOT continue the evaluation without real RAPL.
 * ------------------------------------------------------------------------- */
int rapl_init(rapl_meter_t *meter) {
    if (!meter) return -1;
    memset(meter, 0, sizeof(*meter));
    meter->pkg_fd  = -1;
    meter->core_fd = -1;

    /* ---- Find package domain ---- */
    char pkg_energy_path[MAX_PATH_LEN] = {0};
    if (rapl_find_domain_path("package", pkg_energy_path, sizeof(pkg_energy_path)) != 0) {
        /*
         * Package domain not found.  This means intel_rapl_msr (or
         * intel_rapl_common) is not loaded, or the sysfs tree is not
         * reachable with the current permissions.
         *
         * Do NOT fall back to estimated wattages — that would fabricate data.
         */
        fprintf(stderr,
            "\n"
            "[FATAL] Intel RAPL package domain not found under %s/intel-rapl/\n"
            "\n"
            "  Real energy measurement is REQUIRED; estimated wattages are FORBIDDEN.\n"
            "\n"
            "  To fix:\n"
            "    sudo modprobe intel_rapl_msr\n"
            "    ls %s/intel-rapl/      # verify domain dirs appear\n"
            "    # Ensure the energy_uj files are world-readable, or run as root.\n"
            "\n",
            POWERCAP_ROOT, POWERCAP_ROOT);
        return -1;
    }

    snprintf(meter->pkg_path, sizeof(meter->pkg_path), "%s", pkg_energy_path);
    LOG_INFO("RAPL package domain: %s", meter->pkg_path);

    /* ---- Read max_energy_range_uj (needed for wraparound) ---- */
    {
        /* Derive the max_energy_range_uj path from the energy_uj path */
        char max_path[MAX_PATH_LEN];
        snprintf(max_path, sizeof(max_path), "%s", meter->pkg_path);
        char *slash = strrchr(max_path, '/');
        if (slash) {
            strcpy(slash + 1, "max_energy_range_uj");
            uint64_t mr = read_uj_from_path(max_path);
            if (mr > 0) {
                meter->max_range_uj = mr;
                LOG_INFO("RAPL max_energy_range_uj = %" PRIu64 " uJ", mr);
            } else {
                LOG_WARN("Could not read max_energy_range_uj from %s — "
                         "wraparound will be detected but correction may be inaccurate.",
                         max_path);
                meter->max_range_uj = 0;  /* 0 means wraparound → error, see below */
            }
        }
    }

    meter->pkg_fd = open(meter->pkg_path, O_RDONLY);
    if (meter->pkg_fd < 0) {
        fprintf(stderr,
            "[FATAL] Cannot open RAPL energy_uj (%s): errno=%d (%s).\n"
            "  Check read permissions (chmod a+r or run as root).\n",
            meter->pkg_path, errno, strerror(errno));
        return -1;
    }

    /* ---- Find core/PP0 sub-domain (optional — no hard failure if absent) ---- */
    {
        char pkg_dir[MAX_PATH_LEN];
        snprintf(pkg_dir, sizeof(pkg_dir), "%s", meter->pkg_path);
        char *slash = strrchr(pkg_dir, '/');
        if (slash) *slash = '\0';   /* strip /energy_uj → package domain dir */

        char core_path[MAX_PATH_LEN] = {0};
        if (rapl_find_subdomain_path(pkg_dir, core_path, sizeof(core_path)) == 0) {
            snprintf(meter->core_path, sizeof(meter->core_path), "%s", core_path);
            meter->core_fd = open(meter->core_path, O_RDONLY);
            if (meter->core_fd >= 0) {
                LOG_INFO("RAPL core/PP0 sub-domain: %s", meter->core_path);
            } else {
                LOG_WARN("RAPL core sub-domain found (%s) but not readable: %s",
                         meter->core_path, strerror(errno));
                meter->core_fd = -1;
            }
        } else {
            LOG_INFO("No RAPL core/PP0 sub-domain found (single-domain package).");
        }
    }

    meter->prev_pkg_uj  = read_uj_from_fd(meter->pkg_fd);
    meter->prev_core_uj = (meter->core_fd >= 0) ? read_uj_from_fd(meter->core_fd) : 0;
    meter->prev_timestamp_ns = get_time_ns();
    meter->available = true;

    LOG_INFO("Intel RAPL initialised. Package start = %" PRIu64 " uJ.", meter->prev_pkg_uj);
    return 0;
}

void rapl_close(rapl_meter_t *meter) {
    if (!meter) return;
    if (meter->pkg_fd  >= 0) { close(meter->pkg_fd);  meter->pkg_fd  = -1; }
    if (meter->core_fd >= 0) { close(meter->core_fd); meter->core_fd = -1; }
    meter->available = false;
}

/* -------------------------------------------------------------------------
 * rapl_sample: read current counter, compute delta with wraparound handling.
 *
 * Wraparound: when cur < prev, the counter rolled over at max_range_uj.
 * If max_range_uj is 0 (failed to read it), we treat wraparound as an error
 * rather than guess — the sample is returned with 0 energy and a warning.
 * ------------------------------------------------------------------------- */
int rapl_sample(rapl_meter_t *meter, rapl_sample_t *sample) {
    if (!meter || !sample) return -1;
    memset(sample, 0, sizeof(*sample));

    if (!meter->available || meter->pkg_fd < 0) {
        fprintf(stderr,
            "[FATAL] rapl_sample called but RAPL is unavailable. "
            "rapl_init() must succeed before sampling. Aborting.\n");
        return -1;
    }

    uint64_t now_ns = get_time_ns();
    double dt_sec = (double)(now_ns - meter->prev_timestamp_ns) / 1e9;
    if (dt_sec <= 0.0) dt_sec = 1e-4;
    sample->elapsed_sec = dt_sec;

    uint64_t cur_pkg  = read_uj_from_fd(meter->pkg_fd);
    uint64_t cur_core = (meter->core_fd >= 0) ? read_uj_from_fd(meter->core_fd) : 0;

    /* Package delta with wraparound correction */
    uint64_t d_pkg_uj;
    if (cur_pkg >= meter->prev_pkg_uj) {
        d_pkg_uj = cur_pkg - meter->prev_pkg_uj;
    } else {
        /* Counter wrapped */
        if (meter->max_range_uj > 0) {
            d_pkg_uj = cur_pkg + meter->max_range_uj - meter->prev_pkg_uj;
            LOG_DEBUG("RAPL pkg counter wrapped: prev=%" PRIu64 " cur=%" PRIu64
                      " max=%" PRIu64 " delta=%" PRIu64,
                      meter->prev_pkg_uj, cur_pkg, meter->max_range_uj, d_pkg_uj);
        } else {
            LOG_WARN("RAPL pkg counter wrapped but max_range_uj=0 — "
                     "cannot correct. Skipping this sample (energy=0).");
            d_pkg_uj = 0;
        }
    }

    /* Core delta */
    uint64_t d_core_uj = 0;
    if (meter->core_fd >= 0) {
        if (cur_core >= meter->prev_core_uj) {
            d_core_uj = cur_core - meter->prev_core_uj;
        } else if (meter->max_range_uj > 0) {
            d_core_uj = cur_core + meter->max_range_uj - meter->prev_core_uj;
        }
    }

    meter->prev_pkg_uj       = cur_pkg;
    meter->prev_core_uj      = cur_core;
    meter->prev_timestamp_ns = now_ns;

    sample->delta_pkg_joules    = (double)d_pkg_uj  / 1e6;
    sample->delta_core_joules   = (double)d_core_uj / 1e6;
    sample->avg_pkg_power_watts  = sample->delta_pkg_joules  / dt_sec;
    sample->avg_core_power_watts = sample->delta_core_joules / dt_sec;
    return 0;
}

double rapl_calculate_edp(double joules, double duration_sec) {
    if (joules       < 0.0) joules       = 0.0;
    if (duration_sec < 0.0) duration_sec = 0.0;
    return joules * duration_sec;
}

#pragma GCC diagnostic pop
