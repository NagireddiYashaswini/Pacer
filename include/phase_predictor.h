#ifndef PHASE_PREDICTOR_H
#define PHASE_PREDICTOR_H

#include "common.h"

#define PRED_NUM_PHASES 2
#define PRED_PHASE_COMPUTE 0
#define PRED_PHASE_MEMORY  1

#define DEFAULT_DWELL_BETA 0.70
#define DEFAULT_PRIOR_ALPHA 0.50
#define DEFAULT_MIGRATION_COST_MS 15.0  /* Equivalent time penalty for cache flush & migration in ms */
#define DEFAULT_MIN_PROFITABLE_DWELL_MS 120.0 /* Minimum dwell time for migration to break even */

typedef struct {
    /* Online Markov transition frequency matrix: N[from][to] */
    double transition_counts[PRED_NUM_PHASES][PRED_NUM_PHASES];
    double total_transitions_from[PRED_NUM_PHASES];

    /* Dwell time statistics (Exponential Moving Average in milliseconds) */
    double avg_dwell_ms[PRED_NUM_PHASES];
    double current_phase_start_time_ms;
    double current_phase_dwell_ms;
    int current_phase_ticks;

    /* Current and last classified state */
    int current_phase;   /* PRED_PHASE_COMPUTE or PRED_PHASE_MEMORY */
    int last_phase;
    bool initialized;

    /* History window for n-gram / trend detection */
    uint32_t phase_history_bits; /* bitwise history of last 32 classifications */

    /* Model Hyperparameters */
    double alpha;            /* Laplace smoothing prior */
    double beta;             /* EMA decay factor for dwell times */
    double migration_cost_ms;/* Cost of migration in ms-equivalent EDP penalty */
    double min_profitable_dwell_ms;
    double confidence_threshold;

    /* Online Accuracy & Decision Telemetry */
    uint64_t total_predictions;
    uint64_t correct_predictions;
    uint64_t migrations_recommended;
    uint64_t migrations_suppressed_cost;
    uint64_t migrations_suppressed_uncertainty;
} phase_predictor_t;

/* Recommendation structure returned to the scheduler daemon */
typedef struct {
    int current_phase;
    int predicted_next_phase;
    double transition_probability;
    double expected_dwell_ms;
    double confidence;
    bool phase_transition_imminent;
    bool migration_profitable;
    core_type_t recommended_core_type;
    const char *reason;
} prediction_decision_t;

/* Predictor API */
void predictor_init(phase_predictor_t *pred, double alpha, double beta, double mig_cost_ms);
void predictor_reset(phase_predictor_t *pred);

/* Classifies current tick metrics (IPC, MPKI) and updates online models */
void predictor_update_metrics(phase_predictor_t *pred, double ipc, double mpki,
                              double tick_interval_ms, double current_time_ms);

/* Queries the predictor to evaluate proactive migration for the thread */
prediction_decision_t predictor_evaluate_scheduling(phase_predictor_t *pred,
                                                    core_type_t current_core_type);

/* Helper string conversion */
static inline const char *predictor_phase_to_str(int phase) {
    switch (phase) {
        case PRED_PHASE_COMPUTE: return "COMPUTE";
        case PRED_PHASE_MEMORY:  return "MEMORY";
        default:                 return "UNKNOWN";
    }
}

#endif /* PHASE_PREDICTOR_H */
