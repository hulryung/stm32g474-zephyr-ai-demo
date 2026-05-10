#include "dual_ekf.h"

#include <math.h>
#include <string.h>

/* SOC-OCV table from training data — copied from ai_bms_soc.
 * (Hardcoded for the demo since we only have one cell to track.)
 */
static const float OCV_SOC[21] = {
     0.00f,  5.00f, 10.00f, 15.00f, 20.00f, 25.00f, 30.00f,
    35.00f, 40.00f, 45.00f, 50.00f, 55.00f, 60.00f, 65.00f,
    70.00f, 75.00f, 80.00f, 85.00f, 90.00f, 95.00f, 100.00f,
};
static const float OCV_V[21] = {
    3.355f, 3.501f, 3.583f, 3.620f, 3.643f, 3.660f, 3.677f,
    3.694f, 3.712f, 3.732f, 3.755f, 3.781f, 3.812f, 3.847f,
    3.886f, 3.929f, 3.978f, 4.033f, 4.094f, 4.156f, 4.193f,
};

static float ocv_at_soc(float soc) {
    if (soc <= OCV_SOC[0])  return OCV_V[0];
    for (int i = 1; i < 21; ++i) {
        if (soc <= OCV_SOC[i]) {
            float a = (soc - OCV_SOC[i-1]) / (OCV_SOC[i] - OCV_SOC[i-1]);
            return OCV_V[i-1] + a * (OCV_V[i] - OCV_V[i-1]);
        }
    }
    return OCV_V[20];
}

static float docv_dsoc(float soc) {
    const float h = 0.5f;
    return (ocv_at_soc(soc + h) - ocv_at_soc(soc - h)) / (2.0f * h);
}

void dual_ekf_init(struct dual_ekf *e, float soc0, float Q_initial,
                   float R0, float R1, float C1)
{
    e->soc_pct = soc0;
    e->v_rc = 0.0f;
    e->P_soc = 1.0f;
    e->P_vrc = 0.01f;
    e->Q_estimate_ah = Q_initial;
    e->P_Q = 0.05f;
    e->ah_passed_in_cycle = 0.0f;
    e->soc_at_cycle_start = soc0;
    e->R0 = R0;
    e->R1 = R1;
    e->C1 = C1;
}

void dual_ekf_step(struct dual_ekf *e, float v_meas, float i_a, float dt_s)
{
    /* --- accumulate Ah for slow EKF --- */
    e->ah_passed_in_cycle += fabsf(i_a) * dt_s / 3600.0f;

    /* --- fast EKF: predict --- */
    /* SOC[%] += (i * dt / 3600 / Q) * 100  ;  i<0 discharge → SOC down */
    float dSOC = i_a * dt_s / (e->Q_estimate_ah * 36.0f);
    float a    = 1.0f - dt_s / (e->R1 * e->C1);
    if (a < 0.0f) a = 0.0f;
    float soc_pred = e->soc_pct + dSOC;
    float vrc_pred = a * e->v_rc + (dt_s / e->C1) * i_a;

    /* P_pred (diagonal approximation — full 2x2 wasn't worth the math given
     * the small cross-terms after the OCV linearization) */
    float P_soc_pred = e->P_soc + 0.001f;       /* Q_proc_soc */
    float P_vrc_pred = a*a*e->P_vrc + 0.0001f;  /* Q_proc_vrc */

    /* --- fast EKF: update with V measurement --- */
    float ocv     = ocv_at_soc(soc_pred);
    float h_dsoc  = docv_dsoc(soc_pred);
    float z_pred  = ocv - i_a * e->R0 - vrc_pred;
    float innov   = v_meas - z_pred;

    /* Innovation covariance (ignoring cross-cov) */
    float Rmeas = 0.01f;
    float S = h_dsoc*h_dsoc*P_soc_pred + P_vrc_pred + Rmeas;
    float K_soc = (P_soc_pred * h_dsoc) / S;
    float K_vrc = (-P_vrc_pred)         / S;

    e->soc_pct = soc_pred + K_soc * innov;
    e->v_rc    = vrc_pred + K_vrc * innov;
    e->P_soc   = (1.0f - K_soc * h_dsoc) * P_soc_pred;
    e->P_vrc   = (1.0f - K_vrc * (-1.0f)) * P_vrc_pred;
    if (e->P_soc < 1e-6f) e->P_soc = 1e-6f;
    if (e->P_vrc < 1e-6f) e->P_vrc = 1e-6f;

    /* clip SOC */
    if (e->soc_pct < 0.0f)   e->soc_pct = 0.0f;
    if (e->soc_pct > 100.0f) e->soc_pct = 100.0f;
}

void dual_ekf_end_of_cycle(struct dual_ekf *e, float cycle_ended_at_soc)
{
    /* Slow EKF measurement: predicted_Ah = (SOC_start - SOC_end) * Q / 100
     * observed_Ah = e->ah_passed_in_cycle
     * If predicted < observed → Q_estimate is too low → bump it up.
     * If predicted > observed → Q_estimate is too high → bump it down.
     */
    float dSOC = e->soc_at_cycle_start - cycle_ended_at_soc;
    if (dSOC < 5.0f) {
        /* Skip cycles too short to give us a reliable Q signal. */
        e->ah_passed_in_cycle = 0.0f;
        e->soc_at_cycle_start = e->soc_pct;
        return;
    }
    float predicted_Ah = (dSOC / 100.0f) * e->Q_estimate_ah;
    float innov = e->ah_passed_in_cycle - predicted_Ah;

    /* H = dSOC/100 (sensitivity of predicted Ah to Q) */
    float H = dSOC / 100.0f;
    float Rmeas_Q = 0.005f;            /* 5 mAh measurement noise */
    float Q_proc  = 0.0001f;           /* Q drifts ~mAh/cycle */
    float P_pred = e->P_Q + Q_proc;
    float S = H*H*P_pred + Rmeas_Q;
    float K = (P_pred * H) / S;

    e->Q_estimate_ah += K * innov;
    e->P_Q = (1.0f - K * H) * P_pred;
    if (e->P_Q < 1e-6f) e->P_Q = 1e-6f;
    /* Sanity clip — capacity can't grow with use */
    if (e->Q_estimate_ah < 0.5f)  e->Q_estimate_ah = 0.5f;
    if (e->Q_estimate_ah > 3.0f)  e->Q_estimate_ah = 3.0f;

    /* Reset per-cycle integrator */
    e->ah_passed_in_cycle = 0.0f;
    e->soc_at_cycle_start = e->soc_pct;
}

float dual_ekf_soc(const struct dual_ekf *e)      { return e->soc_pct; }
float dual_ekf_capacity(const struct dual_ekf *e) { return e->Q_estimate_ah; }
float dual_ekf_soh_pct(const struct dual_ekf *e, float Q_nominal) {
    return 100.0f * e->Q_estimate_ah / Q_nominal;
}
