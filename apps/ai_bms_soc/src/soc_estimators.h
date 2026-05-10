/*
 * Common interface for the six SOC estimators.
 *
 * Each estimator gets a stream of (V, I, T, dt) samples and returns its
 * SOC estimate at the end of the window. State is kept in opaque structs
 * passed by the caller so each estimator instance is independent.
 */

#ifndef SOC_ESTIMATORS_H
#define SOC_ESTIMATORS_H

#include <stdint.h>
#include "soc_calib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1. Coulomb counting ----------------------------------------------- */
/* Stateful: caller passes the integration accumulator. */
struct cc_state {
    float soc;          /* current SOC in % */
    float dt_total_s;
};
void  soc_cc_reset(struct cc_state *st, float soc0_pct);
float soc_cc_step(struct cc_state *st, float current_a, float dt_s);
float soc_cc_window(const struct soc_sample *s, float soc0_pct);

/* ---- 2. OCV lookup ------------------------------------------------------ */
/* Stateless: takes one V sample, returns SOC. */
float soc_ocv_lookup(float v_term);

/* ---- 3. Extended Kalman Filter ----------------------------------------- */
struct ekf_state {
    float soc;          /* %  */
    float v_rc;         /* V  (RC element voltage) */
    float P[4];         /* 2x2 covariance row-major */
};
void  soc_ekf_init(struct ekf_state *st, float soc0_pct);
float soc_ekf_step(struct ekf_state *st, float v_meas, float i_a, float dt_s);
float soc_ekf_window(const struct soc_sample *s, float soc0_pct);

/* ---- 4. MLP (per-sample inference) ------------------------------------ */
int   soc_mlp_init(void);
float soc_mlp_estimate(float v, float i, float T);
float soc_mlp_window(const struct soc_sample *s);  /* returns SOC at last sample */

/* ---- 5. LSTM (window inference) --------------------------------------- */
int   soc_lstm_init(void);
float soc_lstm_window(const struct soc_sample *s);

/* ---- 6. Hybrid: EKF + MLP residual ----------------------------------- */
int   soc_hybrid_init(void);
float soc_hybrid_window(const struct soc_sample *s, float soc0_pct);

/* Diagnostic: arena sizes / model sizes */
uint32_t soc_mlp_arena_size(void);
uint32_t soc_lstm_arena_size(void);
uint32_t soc_hybrid_arena_size(void);

#ifdef __cplusplus
}
#endif

#endif
