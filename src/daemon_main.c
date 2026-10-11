#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "scheduler_daemon.h"

static scheduler_daemon_t g_daemon;

static void sig_handler(int sig) {
    (void)sig;
    printf("\n");
    LOG_INFO("Received shutdown signal. Stopping scheduler daemon...");
    daemon_stop(&g_daemon);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  --config <path>       Path to daemon.conf (default: config/daemon.conf)\n");
    printf("  --pid <PID>           Target process PID to monitor and schedule\n");
    printf("  --interval <ms>       PMU sampling interval in milliseconds (default: 250)\n");
    printf("  --cooldown <ms>       Anti-thrashing cooldown timer in ms (default: 1500)\n");
    printf("  --hysteresis <ticks>  Hysteresis confirmation ticks (default: 2)\n");
    printf("  --predictive          Enable online phase-transition predictor (Stage 3)\n");
    printf("  --reactive            Disable predictor, force reactive scheduling mode\n");
    printf("  --mig-cost <ms>       Migration cost threshold penalty in ms (default: 15.0)\n");
    printf("  --p-cores <list>      P-core CPU list override (e.g. 0-7)\n");
    printf("  --e-cores <list>      E-core CPU list override (e.g. 8-17)\n");
    printf("  --help                Display this help\n");
}

int main(int argc, char **argv) {
    const char *config_file = "config/daemon.conf";
    pid_t target_pid = -1;
    int interval_override = -1;
    int cooldown_override = -1;
    int hysteresis_override = -1;
    int predictive_mode = -1; /* -1: default from conf, 1: force predictive, 0: force reactive */
    double mig_cost_override = -1.0;
    const char *p_cores_arg = NULL;
    const char *e_cores_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            target_pid = (pid_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cooldown") == 0 && i + 1 < argc) {
            cooldown_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hysteresis") == 0 && i + 1 < argc) {
            hysteresis_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--predictive") == 0) {
            predictive_mode = 1;
        } else if (strcmp(argv[i], "--reactive") == 0) {
            predictive_mode = 0;
        } else if (strcmp(argv[i], "--mig-cost") == 0 && i + 1 < argc) {
            mig_cost_override = atof(argv[++i]);
        } else if (strcmp(argv[i], "--p-cores") == 0 && i + 1 < argc) {
            p_cores_arg = argv[++i];
        } else if (strcmp(argv[i], "--e-cores") == 0 && i + 1 < argc) {
            e_cores_arg = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Register signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("=======================================================================\n");
    printf("  Measured-Speedup-Aware Hybrid CPU Scheduling Daemon (hybridsched)    \n");
    printf("=======================================================================\n");

    if (daemon_init(&g_daemon, config_file) != 0) {
        LOG_ERROR("Daemon initialization failed!");
        return 1;
    }

    /* Apply command-line overrides */
    if (interval_override > 0) g_daemon.config.sampling_interval_ms = interval_override;
    if (cooldown_override > 0) g_daemon.config.cooldown_ms = cooldown_override;
    if (hysteresis_override > 0) g_daemon.config.hysteresis_ticks = hysteresis_override;
    if (predictive_mode != -1) g_daemon.config.enable_predictive_scheduler = (predictive_mode == 1);
    if (mig_cost_override > 0.0) g_daemon.config.migration_cost_ms = mig_cost_override;
    if (p_cores_arg || e_cores_arg) {
        topology_set_manual_masks(&g_daemon.topo, p_cores_arg, e_cores_arg);
    }

    topology_print(&g_daemon.topo);

    if (target_pid > 0) {
        daemon_add_target_pid(&g_daemon, target_pid);
    } else {
        LOG_INFO("No specific target PID specified (--pid). Daemon will scan and monitor workloads.");
    }

    /* Run main daemon sampling & scheduling loop */
    daemon_run_loop(&g_daemon);

    /* Clean shutdown */
    daemon_cleanup(&g_daemon);
    return 0;
}
