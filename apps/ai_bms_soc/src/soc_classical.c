/*
 * Classical SOC estimators: coulomb counting, OCV lookup, EKF.
 * No ML, just Kalman + table lookup + integration.
 */

#include "soc_estimators.h"

#include <math.h>
#include <string.h>

/* ============== 1. Coulomb counting ==================================== */

void soc_cc_reset(struct cc_state *st, float soc0_pct)
{
    st->soc = soc0_pct;
    st->dt_total_s = 0.0f;
}

float soc_cc_step(struct cc_state *st, float current_a, float dt_s)
{
    /* Discharge current is negative; SOC decreases when |i|*dt accumulates. */
    /* SOC[%] -= (Ah passed) / Q_nom * 100  ;  Ah = |i|*dt/3600 */
    float dAh = fabsf(current_a) * dt_s / 3600.0f;
    st->soc -= 100.0f * dAh / g_ecm_Qnom;
    if (st->soc < 0.0f) st->soc = 0.0f;
    if (st->soc > 100.0f) st->soc = 100.0f;
    st->dt_total_s += dt_s;
    return st->soc;
}

float soc_cc_window(const struct soc_sample *s, float soc0_pct)
{
    struct cc_state st;
    soc_cc_reset(&st, soc0_pct);
    /* dt is the cycle's duration / WINDOW; we don't have it stored — assume
     * uniform spacing. NASA discharges typically last ~3000s for 32 samples
     * → ~95 s per step. We approximate from capacity: dt = (Ah / |I|_avg) /
     * window_size * 3600. Good enough for demonstration.
     */
    float i_avg = 0.0f;
    for (int k = 0; k < SOC_WINDOW; ++k) {
        i_avg += fabsf(s->i[k]);
    }
    i_avg /= (float)SOC_WINDOW;
    if (i_avg < 0.05f) i_avg = 1.5f;       /* fallback to typical 2A discharge */
    float dt = (s->capacity_ah * 3600.0f / i_avg) / (float)SOC_WINDOW;

    for (int k = 0; k < SOC_WINDOW; ++k) {
        soc_cc_step(&st, s->i[k], dt);
    }
    return st.soc;
}

/* ============== 2. OCV lookup ========================================= */

float soc_ocv_lookup(float v_term)
{
    /* Find SOC for given voltage by linear interp on the OCV table.
     * Note: this assumes V_term ≈ V_OC, which only holds at rest. We expose
     * it anyway to demonstrate the bias when used during current flow. */
    if (v_term <= g_sococv_ocv[0]) {
        return g_sococv_soc[0];
    }
    for (unsigned i = 1; i < g_sococv_n; ++i) {
        if (v_term <= g_sococv_ocv[i]) {
            float a = (v_term - g_sococv_ocv[i-1]) /
                      (g_sococv_ocv[i] - g_sococv_ocv[i-1] + 1e-6f);
            return g_sococv_soc[i-1] + a * (g_sococv_soc[i] - g_sococv_soc[i-1]);
        }
    }
    return g_sococv_soc[g_sococv_n - 1];
}

/* ============== 3. EKF ================================================ */

/* Helper: linear interp on the SOC-OCV table */
static float ocv_at_soc(float soc)
{
    if (soc <= g_sococv_soc[0]) {
        return g_sococv_ocv[0];
    }
    for (unsigned i = 1; i < g_sococv_n; ++i) {
        if (soc <= g_sococv_soc[i]) {
            float a = (soc - g_sococv_soc[i-1]) /
                      (g_sococv_soc[i] - g_sococv_soc[i-1]);
            return g_sococv_ocv[i-1] + a * (g_sococv_ocv[i] - g_sococv_ocv[i-1]);
        }
    }
    return g_sococv_ocv[g_sococv_n - 1];
}

static float docv_dsoc(float soc)
{
    /* Numerical derivative of OCV w.r.t. SOC */
    const float h = 0.5f;
    return (ocv_at_soc(soc + h) - ocv_at_soc(soc - h)) / (2.0f * h);
}

void soc_ekf_init(struct ekf_state *st, float soc0_pct)
{
    st->soc = soc0_pct;
    st->v_rc = 0.0f;
    /* P = diag(1, 0.01) — initial uncertainty: ±10% SOC, ±0.1V V_RC */
    st->P[0] = 1.0f;   st->P[1] = 0.0f;
    st->P[2] = 0.0f;   st->P[3] = 0.01f;
}

float soc_ekf_step(struct ekf_state *st, float v_meas, float i_a, float dt_s)
{
    const float Qproc_soc = 0.001f;      /* SOC process noise */
    const float Qproc_vrc = 0.0001f;     /* V_RC process noise */
    const float Rmeas     = 0.01f;       /* measurement noise (V²) */

    /* --- predict ---
     * x[0] (SOC, %) -= |i|*dt/(Q*36)        (sign handled below for charge/disch)
     * x[1] (V_RC)   = V_RC*(1 - dt/τ) + i*dt/C1   ;  τ = R1*C1
     * NASA: discharge current is negative — SOC drops when i<0 too.
     * We use absolute Ah for SOC change so direction of i doesn't bias us.
     */
    float a = 1.0f - dt_s / (g_ecm_R1 * g_ecm_C1);
    if (a < 0.0f) a = 0.0f;
    /* SOC update: SOC[%](t+dt) = SOC[%](t) + (i*dt/3600/Q)*100
     * For NASA-style sign (discharge current is negative): i<0 → dSOC<0,
     * i>0 → dSOC>0. So the multiplier of i is +1/(Q*36), NOT -1/(Q*36). */
    float dSOC = i_a * dt_s / (g_ecm_Qnom * 36.0f);   /* %  ( /36 = /3600*100 ) */
    float soc_pred  = st->soc + dSOC;
    float vrc_pred  = a * st->v_rc + (dt_s / g_ecm_C1) * i_a;

    /* P_pred = F P F^T + Q,  with F = [[1,0],[0,a]] */
    float P00 = st->P[0] + Qproc_soc;
    float P01 = st->P[1] * a;
    float P10 = st->P[2] * a;
    float P11 = a*a*st->P[3] + Qproc_vrc;

    /* --- update --- */
    float ocv = ocv_at_soc(soc_pred);
    float h_dsoc = docv_dsoc(soc_pred);    /* dh/dSOC */
    /* H = [dOCV/dSOC, -1] */
    float z_pred = ocv - i_a * g_ecm_R0 - vrc_pred;
    float innov  = v_meas - z_pred;

    /* S = H P H^T + R   (1x1) */
    /* H P = [h_dsoc, -1] * [[P00,P01],[P10,P11]]
     *     = [h_dsoc*P00 - P10, h_dsoc*P01 - P11] */
    float HP0 = h_dsoc * P00 - P10;
    float HP1 = h_dsoc * P01 - P11;
    float S   = HP0 * h_dsoc + HP1 * (-1.0f) + Rmeas;
    if (S < 1e-9f) S = 1e-9f;

    /* K = P H^T / S = [P00*h_dsoc - P01, P10*h_dsoc - P11] / S */
    float K0 = (P00 * h_dsoc - P01) / S;
    float K1 = (P10 * h_dsoc - P11) / S;

    /* x = x_pred + K * innov */
    st->soc  = soc_pred  + K0 * innov;
    st->v_rc = vrc_pred  + K1 * innov;

    /* P = (I - K H) P_pred */
    /* I - K H = [[1 - K0*h_dsoc,  K0],[ -K1*h_dsoc, 1 + K1]] */
    float A00 = 1.0f - K0 * h_dsoc;
    float A01 = K0;
    float A10 = -K1 * h_dsoc;
    float A11 = 1.0f + K1;
    st->P[0] = A00 * P00 + A01 * P10;
    st->P[1] = A00 * P01 + A01 * P11;
    st->P[2] = A10 * P00 + A11 * P10;
    st->P[3] = A10 * P01 + A11 * P11;

    /* clip SOC */
    if (st->soc < 0.0f) st->soc = 0.0f;
    if (st->soc > 100.0f) st->soc = 100.0f;
    return st->soc;
}

float soc_ekf_window(const struct soc_sample *s, float soc0_pct)
{
    struct ekf_state st;
    soc_ekf_init(&st, soc0_pct);

    /* Same dt approximation as coulomb counting. */
    float i_avg = 0.0f;
    for (int k = 0; k < SOC_WINDOW; ++k) i_avg += fabsf(s->i[k]);
    i_avg /= (float)SOC_WINDOW;
    if (i_avg < 0.05f) i_avg = 1.5f;
    float dt = (s->capacity_ah * 3600.0f / i_avg) / (float)SOC_WINDOW;

    for (int k = 0; k < SOC_WINDOW; ++k) {
        soc_ekf_step(&st, s->v[k], s->i[k], dt);
    }
    return st.soc;
}
