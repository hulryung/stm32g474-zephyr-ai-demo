/*
 * Dual Extended Kalman Filter — SOC + Q (capacity) co-estimation.
 *
 * Two coupled estimators:
 *   - FAST EKF (run every step):  state = [SOC, V_RC]
 *   - SLOW EKF (run every cycle): state = [Q]   (capacity tracker)
 *
 * Coupling: the SLOW EKF's measurement is the actual ampere-hours
 * delivered during a full discharge cycle. We compare it against the
 * predicted Ah (= ΔSOC × Q_estimate / 100) and update Q accordingly.
 *
 * After many cycles, Q_estimate converges to the cell's true degraded
 * capacity, and the FAST EKF's SOC accuracy stops drifting because it's
 * now using the right denominator.
 */

#ifndef DUAL_EKF_H
#define DUAL_EKF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dual_ekf {
    /* Fast EKF state */
    float soc_pct;            /* %  */
    float v_rc;               /* V  */
    float P_soc;              /* covariance, SOC channel */
    float P_vrc;
    /* Slow EKF state */
    float Q_estimate_ah;      /* current capacity estimate */
    float P_Q;                /* covariance on Q */
    /* Per-cycle integrator */
    float ah_passed_in_cycle; /* Ah accumulated since last reset */
    float soc_at_cycle_start;
    /* ECM params (passed in at init) */
    float R0, R1, C1;
};

void dual_ekf_init(struct dual_ekf *e, float soc0, float Q_initial,
                   float R0, float R1, float C1);

/* Per-sample step (call at 100 Hz / 1 kHz). */
void dual_ekf_step(struct dual_ekf *e, float v_meas, float i_a, float dt_s);

/* Mark the end of a full discharge cycle.
 *  - true_ah_delivered: how many Ah actually came out (measured by us, the
 *    integration of |i|*dt over the cycle).
 *  - cycle_ended_at_soc: typically 0 % (full discharge to cutoff).
 *  - Updates Q_estimate based on the discrepancy between observed Ah and
 *    predicted (ΔSOC × Q_estimate / 100).
 */
void dual_ekf_end_of_cycle(struct dual_ekf *e, float cycle_ended_at_soc);

/* Read-only accessors. */
float dual_ekf_soc(const struct dual_ekf *e);
float dual_ekf_capacity(const struct dual_ekf *e);
float dual_ekf_soh_pct(const struct dual_ekf *e, float Q_nominal);

#ifdef __cplusplus
}
#endif
#endif
