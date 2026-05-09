/* Generated — DO NOT EDIT */
#ifndef CYCLES_DB_H
#define CYCLES_DB_H
#include <stddef.h>

#define RUL_WINDOW 32

struct rul_cycle {
    const char  *name;
    const char  *cell;
    unsigned int index;
    float        capacity_ah;
    unsigned int true_rul;     /* ground truth: cycles remaining */
    float        voltage_norm[RUL_WINDOW];
};

extern const unsigned int g_cycle_count;
extern const struct rul_cycle g_cycles[];
#endif
