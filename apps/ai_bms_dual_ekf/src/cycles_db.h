/* Generated — DO NOT EDIT */
#ifndef CYCLES_DB_H
#define CYCLES_DB_H

#define CYCLE_WINDOW 32

#ifdef __cplusplus
extern "C" {
#endif

struct cycle_record {
    unsigned int index;       /* 0..n original cycle number in B0005 */
    float        duration_s;
    float        Q_actual;    /* Ah measured by the lab — ground truth */
    float        v[CYCLE_WINDOW];
    float        i[CYCLE_WINDOW];
    float        T[CYCLE_WINDOW];
};

extern const unsigned int        g_cycle_count;
extern const struct cycle_record g_cycles[];

#ifdef __cplusplus
}
#endif
#endif
