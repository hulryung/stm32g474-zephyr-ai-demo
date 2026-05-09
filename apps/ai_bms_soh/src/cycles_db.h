/* Generated — DO NOT EDIT */
#ifndef CYCLES_DB_H
#define CYCLES_DB_H
#include <stddef.h>

#define SOH_WINDOW 32

struct soh_cycle {
    const char  *name;
    const char  *cell;
    unsigned int index;
    float        true_capacity_ah;
    float        voltage_norm[SOH_WINDOW];
};

extern const unsigned int g_cycle_count;
extern const struct soh_cycle g_cycles[];
#endif
