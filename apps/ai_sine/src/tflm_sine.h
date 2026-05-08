/*
 * C-callable wrapper around the TFLM sine model.
 * Implementation in tflm_sine.cpp (C++ — TFLM is C++).
 */

#ifndef TFLM_SINE_H
#define TFLM_SINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize interpreter and allocate tensors. Returns 0 on success. */
int tflm_sine_init(void);

/* Run one inference. x is in radians (the model was trained on [0, 2π]).
 * On success, *y is the model's predicted sin(x). Returns 0 on success.
 */
int tflm_sine_infer(float x, float *y);

/* Tensor arena size in bytes (for reporting). */
uint32_t tflm_sine_arena_size(void);

#ifdef __cplusplus
}
#endif

#endif /* TFLM_SINE_H */
