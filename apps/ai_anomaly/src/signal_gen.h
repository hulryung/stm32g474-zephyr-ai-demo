/*
 * On-device signal generator + injectable fault state.
 *
 * Used so the demo can run without any external hardware: every cycle we
 * synthesize one 32-sample window of a "normal" signal (matching what the
 * autoencoder was trained on), optionally perturbed by user-set faults
 * issued from shell commands.
 */

#ifndef SIGNAL_GEN_H
#define SIGNAL_GEN_H

#include "tflm_anomaly.h"

#ifdef __cplusplus
extern "C" {
#endif

enum injection_kind {
	INJECTION_NONE  = 0,
	INJECTION_PULSE = 1,   /* 3-sample bump in the middle of the window */
	INJECTION_DRIFT = 2,   /* constant offset across the entire window */
	INJECTION_NOISE = 3,   /* extra random noise on top */
};

void signal_gen_init(void);

/* Generate one window in `out` (length TFLM_ANOMALY_WINDOW). */
void signal_gen_next(float *out);

/* Configure / clear injection. amp is the magnitude (0 disables). */
void signal_gen_set_injection(enum injection_kind kind, float amp);

/* Read back current injection state for the shell `anomaly state` cmd. */
void signal_gen_get_injection(enum injection_kind *kind, float *amp);

#ifdef __cplusplus
}
#endif

#endif
