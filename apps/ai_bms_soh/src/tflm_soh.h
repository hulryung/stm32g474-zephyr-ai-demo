#ifndef TFLM_SOH_H
#define TFLM_SOH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFLM_SOH_WINDOW 32

int      tflm_soh_init(void);

/* window: normalized voltage curve (length 32). Returns predicted capacity
 * in ampere-hours via *cap_out. Returns 0 on success. */
int      tflm_soh_estimate(const float *window, float *cap_out);

uint32_t tflm_soh_arena_size(void);

#ifdef __cplusplus
}
#endif

#endif
