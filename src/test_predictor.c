#include "phase_predictor.h"
#include <assert.h>
#include <stdio.h>

void test_markov_learning_and_dwell(void) {
    printf("[TEST] Testing Markov transition learning and dwell EMA convergence...\n");
    phase_predictor_t pred;
    predictor_init(&pred, 0.5, 0.7, 15.0);

    double current_time = 0.0;
    double tick_ms = 50.0;

    /* Simulate 8 alternating phases of 400ms each */
    for (int phase = 0; phase < 8; phase++) {
        bool is_comp = (phase % 2 == 0);
        double ipc = is_comp ? 2.5 : 0.3;
        double mpki = is_comp ? 0.5 : 25.0;

        for (int t = 0; t < 8; t++) { /* 8 ticks * 50ms = 400ms */
            predictor_update_metrics(&pred, ipc, mpki, tick_ms, current_time);
            current_time += tick_ms;
        }
    }

    /* Verify transition counts */
    printf("   Transition counts: C->C=%.1f, C->M=%.1f | M->M=%.1f, M->C=%.1f\n",
           pred.transition_counts[0][0], pred.transition_counts[0][1],
           pred.transition_counts[1][1], pred.transition_counts[1][0]);

    assert(pred.transition_counts[0][1] >= 3.0);
    assert(pred.transition_counts[1][0] >= 3.0);

    /* Verify dwell time convergence towards 400ms */
    printf("   Learned avg dwell times: Compute=%.1f ms, Memory=%.1f ms\n",
           pred.avg_dwell_ms[PRED_PHASE_COMPUTE], pred.avg_dwell_ms[PRED_PHASE_MEMORY]);

    assert(pred.avg_dwell_ms[PRED_PHASE_COMPUTE] < 450.0);
    assert(pred.avg_dwell_ms[PRED_PHASE_COMPUTE] > 350.0);
    printf("   [PASS] Markov probabilities and dwell EMAs verified!\n");
}

void test_migration_cost_veto(void) {
    printf("\n[TEST] Testing migration cost veto for short dwell times...\n");
    phase_predictor_t pred;
    predictor_init(&pred, 0.5, 0.7, 15.0);
    /* Set learned dwell to very short duration: 60 ms */
    pred.avg_dwell_ms[PRED_PHASE_COMPUTE] = 60.0;
    pred.avg_dwell_ms[PRED_PHASE_MEMORY] = 60.0;
    pred.initialized = true;
    pred.current_phase = PRED_PHASE_COMPUTE;
    pred.current_phase_dwell_ms = 50.0;
    pred.transition_counts[0][1] = 10.0; /* high switch probability */
    pred.total_transitions_from[0] = 10.0;

    /* Evaluating scheduling when on P-core (optimal for compute, but imminent switch to memory) */
    prediction_decision_t dec = predictor_evaluate_scheduling(&pred, CORE_TYPE_P);

    printf("   Decision for 60ms phase: Profitable=%d, Reason='%s'\n",
           dec.migration_profitable, dec.reason);

    /* Should be vetoed because 60ms < min_profitable_dwell_ms (120ms) */
    assert(!dec.migration_profitable);
    assert(pred.migrations_suppressed_cost > 0);
    printf("   [PASS] Migration cost veto correctly suppressed unprofitable migration!\n");
}

void test_proactive_migration_approval(void) {
    printf("\n[TEST] Testing proactive migration approval for long dwell times...\n");
    phase_predictor_t pred;
    predictor_init(&pred, 0.5, 0.7, 15.0);
    /* Set learned dwell to 1000 ms */
    pred.avg_dwell_ms[PRED_PHASE_COMPUTE] = 1000.0;
    pred.avg_dwell_ms[PRED_PHASE_MEMORY] = 1000.0;
    pred.initialized = true;
    pred.current_phase = PRED_PHASE_MEMORY;
    pred.current_phase_dwell_ms = 800.0; /* 80% elapsed */
    pred.transition_counts[1][0] = 8.0;   /* High confidence switch to compute */
    pred.total_transitions_from[1] = 8.0;

    /* Currently on E-core, about to switch to Compute */
    prediction_decision_t dec = predictor_evaluate_scheduling(&pred, CORE_TYPE_E);

    printf("   Decision for 1000ms phase: Profitable=%d, Recommended Core=%s, Reason='%s'\n",
           dec.migration_profitable, core_type_to_str(dec.recommended_core_type), dec.reason);

    assert(dec.migration_profitable);
    assert(dec.recommended_core_type == CORE_TYPE_P);
    assert(dec.phase_transition_imminent);
    printf("   [PASS] Proactive migration successfully approved!\n");
}

int main(void) {
    printf("================ RUNNING PHASE PREDICTOR UNIT TESTS ================\n");
    test_markov_learning_and_dwell();
    test_migration_cost_veto();
    test_proactive_migration_approval();
    printf("================ ALL UNIT TESTS PASSED SUCCESSFULLY ================\n");
    return 0;
}
