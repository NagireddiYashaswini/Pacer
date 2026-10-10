#include "topology.h"
#include <ctype.h>

int parse_cpulist_to_cpuset(const char *cpulist, cpu_set_t *set) {
    if (!cpulist || !set) return -1;
    CPU_ZERO(set);

    const char *p = cpulist;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;

        char *end = NULL;
        long start = strtol(p, &end, 10);
        if (p == end) return -1;
        p = end;

        long last = start;
        if (*p == '-') {
            p++;
            last = strtol(p, &end, 10);
            if (p == end) return -1;
            p = end;
        }

        if (start < 0 || last >= MAX_CPUS || start > last) {
            return -1;
        }

        for (long c = start; c <= last; c++) {
            CPU_SET(c, set);
        }
    }
    return 0;
}

void cpuset_to_str(const cpu_set_t *set, int max_cpu, char *buf, size_t buflen) {
    if (!set || !buf || buflen == 0) return;
    buf[0] = '\0';
    size_t off = 0;
    int start = -1;

    for (int i = 0; i <= max_cpu; i++) {
        bool in_set = (i < max_cpu) ? CPU_ISSET(i, set) : false;
        if (in_set && start < 0) {
            start = i;
        } else if (!in_set && start >= 0) {
            int end = i - 1;
            int written;
            if (start == end) {
                written = snprintf(buf + off, (off < buflen ? buflen - off : 0),
                                   (off > 0 ? ",%d" : "%d"), start);
            } else {
                written = snprintf(buf + off, (off < buflen ? buflen - off : 0),
                                   (off > 0 ? ",%d-%d" : "%d-%d"), start, end);
            }
            if (written > 0) off += written;
            start = -1;
        }
    }
    if (off == 0 && buflen > 4) {
        strncpy(buf, "none", buflen);
    }
}

static int read_sysfs_string(const char *path, char *buf, size_t buflen) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, buflen - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    /* trim trailing newline */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    return 0;
}

static uint64_t read_sysfs_uint64(const char *path) {
    char buf[64];
    if (read_sysfs_string(path, buf, sizeof(buf)) != 0) return 0;
    return (uint64_t)strtoull(buf, NULL, 10);
}

int topology_detect_sysfs_hybrid(cpu_topology_t *topo) {
    char p_buf[256];
    char e_buf[256];

    /* Check for Alder Lake / Raptor Lake sysfs hybrid PMUs */
    int has_p = read_sysfs_string("/sys/devices/cpu_core/cpus", p_buf, sizeof(p_buf));
    int has_e = read_sysfs_string("/sys/devices/cpu_atom/cpus", e_buf, sizeof(e_buf));

    if (has_p == 0 && has_e == 0) {
        CPU_ZERO(&topo->p_core_mask);
        CPU_ZERO(&topo->e_core_mask);
        parse_cpulist_to_cpuset(p_buf, &topo->p_core_mask);
        parse_cpulist_to_cpuset(e_buf, &topo->e_core_mask);
        topo->is_hybrid_hardware = true;
        LOG_INFO("Hardware Hybrid PMU detected! P-cores: %s, E-cores: %s", p_buf, e_buf);
        return 0;
    }

    return -1;
}

static void parse_config_line(cpu_topology_t *topo, const char *line) {
    char key[64], val[256];
    if (sscanf(line, " %63[^= ] = %255s", key, val) == 2) {
        if (strcmp(key, "p_cores") == 0) {
            parse_cpulist_to_cpuset(val, &topo->p_core_mask);
        } else if (strcmp(key, "e_cores") == 0) {
            parse_cpulist_to_cpuset(val, &topo->e_core_mask);
        }
    }
}

int topology_set_manual_masks(cpu_topology_t *topo, const char *p_list_str, const char *e_list_str) {
    if (!topo) return -1;
    if (p_list_str && parse_cpulist_to_cpuset(p_list_str, &topo->p_core_mask) != 0) {
        LOG_ERROR("Failed to parse manual P-core list: %s", p_list_str);
        return -1;
    }
    if (e_list_str && parse_cpulist_to_cpuset(e_list_str, &topo->e_core_mask) != 0) {
        LOG_ERROR("Failed to parse manual E-core list: %s", e_list_str);
        return -1;
    }

    /* Recount */
    topo->num_p_cores = 0;
    topo->num_e_cores = 0;
    for (int i = 0; i < topo->total_cpus; i++) {
        if (CPU_ISSET(i, &topo->p_core_mask)) {
            topo->core_types[i] = CORE_TYPE_P;
            topo->num_p_cores++;
        } else if (CPU_ISSET(i, &topo->e_core_mask)) {
            topo->core_types[i] = CORE_TYPE_E;
            topo->num_e_cores++;
        } else {
            topo->core_types[i] = CORE_TYPE_UNKNOWN;
        }
    }
    return 0;
}

int topology_init(cpu_topology_t *topo, const char *config_file) {
    if (!topo) return -1;
    memset(topo, 0, sizeof(*topo));

    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc <= 0) nproc = 1;
    if (nproc > MAX_CPUS) nproc = MAX_CPUS;
    topo->total_cpus = (int)nproc;

    /* Read frequency characteristics for all CPUs */
    for (int i = 0; i < topo->total_cpus; i++) {
        char freq_path[MAX_PATH_LEN];
        snprintf(freq_path, sizeof(freq_path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        uint64_t freq = read_sysfs_uint64(freq_path);
        if (freq == 0) {
            snprintf(freq_path, sizeof(freq_path),
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
            freq = read_sysfs_uint64(freq_path);
        }
        topo->max_freq_khz[i] = freq;
    }

    /* Try native hybrid detection first */
    if (topology_detect_sysfs_hybrid(topo) == 0) {
        topo->is_hybrid_hardware = true;
    } else {
        topo->is_hybrid_hardware = false;
        LOG_INFO("Native hybrid PMU sysfs not found (homogeneous or virtual CPU detected).");

        /* Try parsing configuration file */
        bool config_loaded = false;
        if (config_file) {
            FILE *f = fopen(config_file, "r");
            if (f) {
                char line[MAX_LINE_LEN];
                while (fgets(line, sizeof(line), f)) {
                    parse_config_line(topo, line);
                }
                fclose(f);
                config_loaded = true;
                LOG_INFO("Loaded P/E core configuration from %s", config_file);
            }
        }

        /* If no valid config masks found, apply balanced partitioning default */
        if (!config_loaded || (CPU_COUNT(&topo->p_core_mask) == 0 && CPU_COUNT(&topo->e_core_mask) == 0)) {
            int half = topo->total_cpus / 2;
            if (half < 1) half = 1;
            CPU_ZERO(&topo->p_core_mask);
            CPU_ZERO(&topo->e_core_mask);
            for (int i = 0; i < half; i++) {
                CPU_SET(i, &topo->p_core_mask);
            }
            for (int i = half; i < topo->total_cpus; i++) {
                CPU_SET(i, &topo->e_core_mask);
            }
            LOG_INFO("Applying default partition: CPUs 0-%d as P-cores, CPUs %d-%d as E-cores",
                     half - 1, half, topo->total_cpus - 1);
        }
    }

    /* Tally core counts and populate array */
    topo->num_p_cores = 0;
    topo->num_e_cores = 0;
    for (int i = 0; i < topo->total_cpus; i++) {
        if (CPU_ISSET(i, &topo->p_core_mask)) {
            topo->core_types[i] = CORE_TYPE_P;
            topo->num_p_cores++;
        } else if (CPU_ISSET(i, &topo->e_core_mask)) {
            topo->core_types[i] = CORE_TYPE_E;
            topo->num_e_cores++;
        } else {
            topo->core_types[i] = CORE_TYPE_UNKNOWN;
        }
    }

    return 0;
}

core_type_t topology_get_core_type(const cpu_topology_t *topo, int cpu_id) {
    if (!topo || cpu_id < 0 || cpu_id >= topo->total_cpus) return CORE_TYPE_UNKNOWN;
    return topo->core_types[cpu_id];
}

bool topology_is_p_core(const cpu_topology_t *topo, int cpu_id) {
    return topology_get_core_type(topo, cpu_id) == CORE_TYPE_P;
}

bool topology_is_e_core(const cpu_topology_t *topo, int cpu_id) {
    return topology_get_core_type(topo, cpu_id) == CORE_TYPE_E;
}

void topology_print(const cpu_topology_t *topo) {
    if (!topo) return;
    char p_str[256], e_str[256];
    cpuset_to_str(&topo->p_core_mask, topo->total_cpus, p_str, sizeof(p_str));
    cpuset_to_str(&topo->e_core_mask, topo->total_cpus, e_str, sizeof(e_str));

    printf("==================== CPU TOPOLOGY SUMMARY ====================\n");
    printf("Total Online CPUs: %d\n", topo->total_cpus);
    printf("Hybrid Hardware:   %s\n", topo->is_hybrid_hardware ? "YES (Native)" : "NO (Emulated/Partitioned)");
    printf("P-Core Pool (%d):  %s\n", topo->num_p_cores, p_str);
    printf("E-Core Pool (%d):  %s\n", topo->num_e_cores, e_str);
    printf("Sample Freq:       CPU 0: %" PRIu64 " kHz | CPU %d: %" PRIu64 " kHz\n",
           topo->max_freq_khz[0],
           (topo->total_cpus > 8 ? 8 : topo->total_cpus - 1),
           topo->max_freq_khz[(topo->total_cpus > 8 ? 8 : topo->total_cpus - 1)]);
    printf("==============================================================\n");
}
