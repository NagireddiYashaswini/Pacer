#include "phase_predictor.h"
#include <math.h>

void predictor_init(phase_predictor_t *pred, double alpha, double beta, double mig_cost_ms) {
    if (!pred) return;
    memset(pred, 0, sizeof(*pred));

    pred->alpha = (alpha > 0.0) ? alpha : DEFAULT_PRIOR_ALPHA;
    pred->beta = (beta > 0.0 && beta < 1.0) ? beta : DEFAULT_DWELL_BETA;
    pred->migration_cost_ms = (mig_cost_ms > 0.0) ? mig_cost_ms : DEFAULT_MIGRATION_COST_MS;
    pred->min_profitable_dwell_ms = DEFAULT_MIN_PROFITABLE_DWELL_MS;
    pred->confidence_threshold = 0.55;

    /* Initialize transition counts with uniform Dirichlet prior */
    for (int i = 0; i < PRED_NUM_PHASES; i++) {
        for (int j = 0; j < PRED_NUM_PHASES; j++) {
            pred->transition_counts[i][j] = 0.0;
        }
        pred->total_transitions_from[i] = 0.0;
        /* Initial prior: 500 ms expected phase duration */
        pred->avg_dwell_ms[i] = 500.0;
    }

    pred->current_phase = PRED_PHASE_COMPUTE;
    pred->last_phase = PRED_PHASE_COMPUTE;
    pred->initialized = false;
}

void predictor_reset(phase_predictor_t *pred) {
    if (!pred) return;
    double a = pred->alpha;
    double b = pred->beta;
    double c = pred->migration_cost_ms;
    predictor_init(pred, a, b, c);
}

void predictor_update_metrics(phase_predictor_t *pred, double ipc, double mpki,
                              double tick_interval_ms, double current_time_ms) {
    if (!pred) return;

    /* Microarchitectural phase classification */
    int observed_phase;
    if (mpki >= 14.0) {
        observed_phase = PRED_PHASE_MEMORY;
    } else if (mpki <= 5.0 && ipc >= 1.2) {
        observed_phase = PRED_PHASE_COMPUTE;
    } else {
        /* Hysteresis zone: maintain current phase unless clear signal */
        observed_phase = pred->initialized ? pred->current_phase : PRED_PHASE_COMPUTE;
    }

    if (!pred->initialized) {
        pred->current_phase = observed_phase;
        pred->last_phase = observed_phase;
        pred->current_phase_start_time_ms = current_time_ms;
        pred->current_phase_dwell_ms = 0.0;
        pred->current_phase_ticks = 1;
        pred->initialized = true;
        return;
    }

    /* Accumulate dwell time */
    pred->current_phase_dwell_ms += tick_interval_ms;
    pred->current_phase_ticks++;

    /* Detect Phase Boundary Shift */
    if (observed_phase != pred->current_phase) {
        int prev = pred->current_phase;
        int next = observed_phase;

        /* Update online transition counts */
        pred->transition_counts[prev][next] += 1.0;
        pred->total_transitions_from[prev] += 1.0;

        /* Update dwell time Exponential Moving Average (EMA) */
        double observed_dwell = pred->current_phase_dwell_ms;
        if (observed_dwell >= 20.0) {
            pred->avg_dwell_ms[prev] = (pred->beta * pred->avg_dwell_ms[prev]) +
                                      ((1.0 - pred->beta) * observed_dwell);
        }

        /* Update bitwise history */
        pred->phase_history_bits = (pred->phase_history_bits << 1) | (next & 1);

        /* Transition to new phase state */
        pred->last_phase = prev;
        pred->current_phase = next;
        pred->current_phase_start_time_ms = current_time_ms;
        pred->current_phase_dwell_ms = 0.0;
        pred->current_phase_ticks = 0;
    }
}

prediction_decision_t predictor_evaluate_scheduling(phase_predictor_t *pred,
                                                    core_type_t current_core_type) {
    prediction_decision_t dec;
    memset(&dec, 0, sizeof(dec));

    if (!pred || !pred->initialized) {
        dec.current_phase = PRED_PHASE_COMPUTE;
        dec.predicted_next_phase = PRED_PHASE_COMPUTE;
        dec.recommended_core_type = current_core_type;
        dec.reason = "Predictor uninitialized";
        return dec;
    }

    int curr = pred->current_phase;
    int other = (curr == PRED_PHASE_COMPUTE) ? PRED_PHASE_MEMORY : PRED_PHASE_COMPUTE;

    dec.current_phase = curr;

    /* Calculate Markov transition probabilities with Laplace smoothing */
    double denom = pred->total_transitions_from[curr] + (PRED_NUM_PHASES * pred->alpha);
    double p_stay   = (pred->transition_counts[curr][curr] + pred->alpha) / denom;
    double p_switch = (pred->transition_counts[curr][other] + pred->alpha) / denom;

    double expected_curr_dwell = pred->avg_dwell_ms[curr];
    double elapsed_dwell = pred->current_phase_dwell_ms;

    /* Dwell horizon and hazard rate analysis:
     * If the thread has dwelled near or beyond the expected phase duration,
     * the hazard rate of switching is elevated. */
    bool dwell_expired = (elapsed_dwell >= 0.70 * expected_curr_dwell);
    bool transition_imminent = (dwell_expired && (p_switch >= 0.45));

    dec.phase_transition_imminent = transition_imminent;

    if (transition_imminent) {
        dec.predicted_next_phase = other;
        dec.transition_probability = p_switch;
        dec.expected_dwell_ms = pred->avg_dwell_ms[other];
        dec.confidence = p_switch;
    } else {
        dec.predicted_next_phase = curr;
        dec.transition_probability = p_stay;
        dec.expected_dwell_ms = fmax(0.0, expected_curr_dwell - elapsed_dwell);
        dec.confidence = p_stay;
    }

    pred->total_predictions++;

    /* Determine optimal core type for the target phase */
    core_type_t optimal_core = (dec.predicted_next_phase == PRED_PHASE_COMPUTE)
                               ? CORE_TYPE_P : CORE_TYPE_E;

    dec.recommended_core_type = optimal_core;

    /* If thread is already running on the optimal core */
    if (optimal_core == current_core_type) {
        dec.migration_profitable = false;
        dec.reason = "Thread already resident on optimal core pool";
        return dec;
    }

    /* -------------------------------------------------------------
     * MIGRATION COST & UNCERTAINTY POLICY
     * Evaluates if predicted target dwell justifies migration cost C_mig
     * ------------------------------------------------------------- */

    /* Check 1: Minimum profitable dwell threshold */
    if (dec.expected_dwell_ms < pred->min_profitable_dwell_ms) {
        pred->migrations_suppressed_cost++;
        dec.migration_profitable = false;
        dec.reason = "VETO: Target dwell too short to amortize cache warm-up cost";
        return dec;
    }

    /* Check 2: Prediction confidence threshold */
    if (dec.confidence < pred->confidence_threshold) {
        pred->migrations_suppressed_uncertainty++;
        dec.migration_profitable = false;
        dec.reason = "VETO: Prediction confidence below uncertainty threshold";
        return dec;
    }

    /* Check 3: Dimensionally consistent Net Expected Gain model
     * Net Gain = P_conf * (1.0 - tau_min / tau_expected) - gamma * (1.0 - P_conf)
     * All terms are strictly dimensionless ratios in [0.0, 1.0]. */
    double amortized_dwell_efficiency = 1.0 - (pred->min_profitable_dwell_ms / dec.expected_dwell_ms);
    if (amortized_dwell_efficiency < 0.0) amortized_dwell_efficiency = 0.0;

    double gamma_uncertainty_weight = 0.35; /* gamma penalty for incorrect prediction */
    double uncertainty_penalty = gamma_uncertainty_weight * (1.0 - dec.confidence);

    double net_expected_gain = (dec.confidence * amortized_dwell_efficiency) - uncertainty_penalty;

    if (net_expected_gain <= 0.0) {
        pred->migrations_suppressed_cost++;
        dec.migration_profitable = false;
        dec.reason = "VETO: Analytic net expected gain <= 0 (uncertainty exceeds amortized dwell efficiency)";
        return dec;
    }

    /* All checks passed: Proactive migration recommended! */
    pred->migrations_recommended++;
    dec.migration_profitable = true;
    dec.reason = "PROACTIVE MIGRATION APPROVED: High-gain predicted phase shift";
    return dec;
}
