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
    printf("  --config <path>          Path to daemon.conf (default: config/daemon.conf)\n");
    printf("  --pid <PID>              Target process PID to monitor and schedule\n");
    printf("  --policy <speedup|mpki>  Scheduling policy (default: speedup)\n");
    printf("  --interval <ms>          PMU sampling interval in ms (default: 250)\n");
    printf("  --cooldown <ms>          Anti-thrashing cooldown timer in ms (default: 1500)\n");
    printf("  --hysteresis <ticks>     Hysteresis confirmation ticks (default: 2)\n");
    printf("  --probe-window <ticks>   Probe measurement window in ticks (default: 2)\n");
    printf("  --discard-ticks <ticks>  Post-migration warm-up ticks to discard (default: 1)\n");
    printf("  --probe-rate-cap <rate>  Max probes/sec/thread (default: 0.5)\n");
    printf("  --decay-alpha <alpha>    EMA decay weight for speedup cache (default: 0.5)\n");
    printf("  --p-cores <list>         P-core CPU list override\n");
    printf("  --e-cores <list>         E-core CPU list override\n");
    printf("  --trial <index>          Trial index for CSV provenance (default: 0)\n");
    printf("  --help                   Display this help\n");
}

int main(int argc, char **argv) {
    const char *config_file = "config/daemon.conf";
    pid_t target_pid = -1;
    int interval_override = -1;
    int cooldown_override = -1;
    int hysteresis_override = -1;
    int probe_window_override = -1;
    int discard_ticks_override = -1;
    double probe_rate_cap_override = -1.0;
    double decay_alpha_override = -1.0;
    int policy_override = -1;
    int trial_override = -1;
    const char *p_cores_arg = NULL;
    const char *e_cores_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            target_pid = (pid_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            if (strcmp(p, "speedup") == 0) policy_override = SCHED_POLICY_SPEEDUP;
            else if (strcmp(p, "mpki") == 0) policy_override = SCHED_POLICY_MPKI;
            else {
                fprintf(stderr, "Unknown --policy '%s' (expected 'speedup' or 'mpki')\n", p);
                return 1;
            }
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cooldown") == 0 && i + 1 < argc) {
            cooldown_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hysteresis") == 0 && i + 1 < argc) {
            hysteresis_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--probe-window") == 0 && i + 1 < argc) {
            probe_window_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--discard-ticks") == 0 && i + 1 < argc) {
            discard_ticks_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--probe-rate-cap") == 0 && i + 1 < argc) {
            probe_rate_cap_override = atof(argv[++i]);
        } else if (strcmp(argv[i], "--decay-alpha") == 0 && i + 1 < argc) {
            decay_alpha_override = atof(argv[++i]);
        } else if (strcmp(argv[i], "--p-cores") == 0 && i + 1 < argc) {
            p_cores_arg = argv[++i];
        } else if (strcmp(argv[i], "--e-cores") == 0 && i + 1 < argc) {
            e_cores_arg = argv[++i];
        } else if (strcmp(argv[i], "--trial") == 0 && i + 1 < argc) {
            trial_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

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

    if (policy_override >= 0) g_daemon.config.policy = (sched_policy_t)policy_override;
    if (interval_override > 0) g_daemon.config.sampling_interval_ms = interval_override;
    if (cooldown_override >= 0) g_daemon.config.cooldown_ms = cooldown_override;
    if (hysteresis_override > 0) g_daemon.config.hysteresis_ticks = hysteresis_override;
    if (probe_window_override > 0) g_daemon.config.probe_window_ticks = probe_window_override;
    if (discard_ticks_override >= 0) g_daemon.config.discard_ticks = discard_ticks_override;
    if (probe_rate_cap_override > 0.0) g_daemon.config.probe_rate_cap = probe_rate_cap_override;
    if (decay_alpha_override > 0.0 && decay_alpha_override <= 1.0) {
        g_daemon.config.decay_alpha = decay_alpha_override;
    }
    if (trial_override >= 0) g_daemon.provenance.trial_index = trial_override;

    if (p_cores_arg || e_cores_arg) {
        topology_set_manual_masks(&g_daemon.topo, p_cores_arg, e_cores_arg);
    }

    LOG_INFO("Policy: %s | ProbeWindow=%d ticks | DiscardTicks=%d | RateCap=%.2f/s | DecayAlpha=%.2f",
             g_daemon.config.policy == SCHED_POLICY_SPEEDUP ? "speedup" : "mpki",
             g_daemon.config.probe_window_ticks,
             g_daemon.config.discard_ticks,
             g_daemon.config.probe_rate_cap,
             g_daemon.config.decay_alpha);

    topology_print(&g_daemon.topo);

    if (target_pid > 0) {
        daemon_add_target_pid(&g_daemon, target_pid);
    } else {
        LOG_INFO("No specific target PID specified (--pid). Daemon will scan and monitor workloads.");
    }

    daemon_run_loop(&g_daemon);
    daemon_cleanup(&g_daemon);
    return 0;
}
