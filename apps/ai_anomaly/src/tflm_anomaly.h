/*
 * C-callable wrapper around the autoencoder.
 * Implementation in tflm_anomaly.cpp.
 *
 * Usage:
 *   tflm_anomaly_init();
 *   float window[32] = ...;        // signal samples in float
 *   float score;
 *   tflm_anomaly_score(window, &score);   // *score = mean squared reconstruction error
 */

#ifndef TFLM_ANOMALY_H
#define TFLM_ANOMALY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFLM_ANOMALY_WINDOW 32

int      tflm_anomaly_init(void);
int      tflm_anomaly_score(const float *window, float *out_score);
uint32_t tflm_anomaly_arena_size(void);

#ifdef __cplusplus
}
#endif

#endif
