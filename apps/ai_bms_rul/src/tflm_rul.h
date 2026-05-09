#ifndef TFLM_RUL_H
#define TFLM_RUL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFLM_RUL_WINDOW 32

int      tflm_rul_init(void);

/* Predict remaining useful life (cycles until capacity < EOL threshold).
 * Returned via *cycles_out as an int. Returns 0 on success. */
int      tflm_rul_predict(const float *window, int *cycles_out);

uint32_t tflm_rul_arena_size(void);

#ifdef __cplusplus
}
#endif

#endif
