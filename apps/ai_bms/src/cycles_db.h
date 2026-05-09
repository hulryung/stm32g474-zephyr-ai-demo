/* Generated — DO NOT EDIT */
#ifndef CYCLES_DB_H
#define CYCLES_DB_H
#include <stddef.h>

#define BMS_CYCLE_LEN 32

struct bms_cycle {
    const char *name;
    const char *cell;
    unsigned int index;
    float capacity_ah;
    float voltage[BMS_CYCLE_LEN];
};

extern const unsigned int g_cycle_count;
extern const struct bms_cycle g_cycles[];
#endif
