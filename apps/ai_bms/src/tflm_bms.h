#ifndef TFLM_BMS_H
#define TFLM_BMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFLM_BMS_WINDOW 32

int      tflm_bms_init(void);
int      tflm_bms_score(const float *window, float *out_score);
uint32_t tflm_bms_arena_size(void);

#ifdef __cplusplus
}
#endif

#endif
