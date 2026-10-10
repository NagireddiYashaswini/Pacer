#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include "common.h"

typedef struct {
    int total_cpus;
    int num_p_cores;
    int num_e_cores;
    cpu_set_t p_core_mask;
    cpu_set_t e_core_mask;
    core_type_t core_types[MAX_CPUS];
    uint64_t max_freq_khz[MAX_CPUS];
    bool is_hybrid_hardware;
} cpu_topology_t;

int topology_init(cpu_topology_t *topo, const char *config_file);
int topology_detect_sysfs_hybrid(cpu_topology_t *topo);
int topology_set_manual_masks(cpu_topology_t *topo, const char *p_list_str, const char *e_list_str);
core_type_t topology_get_core_type(const cpu_topology_t *topo, int cpu_id);
bool topology_is_p_core(const cpu_topology_t *topo, int cpu_id);
bool topology_is_e_core(const cpu_topology_t *topo, int cpu_id);
void topology_print(const cpu_topology_t *topo);

/* CPU list utilities */
int parse_cpulist_to_cpuset(const char *cpulist, cpu_set_t *set);
void cpuset_to_str(const cpu_set_t *set, int max_cpu, char *buf, size_t buflen);

#endif /* TOPOLOGY_H */
