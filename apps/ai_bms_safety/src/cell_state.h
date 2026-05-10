/*
 * Shared cell state — what both threads (safety + ML) read.
 * Updated by the shell command that simulates ADC samples.
 *
 * In a real system this would be filled by the ADC ISR via DMA into a
 * lock-free ring buffer. For the demo we just have one global atomic-ish
 * snapshot updated by `cell sim`.
 */

#ifndef CELL_STATE_H
#define CELL_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N_CELLS 4

struct cell_snapshot {
    float v[N_CELLS];          /* per-cell voltage (V) */
    float i_pack;              /* pack current (A), discharge negative */
    float t_max;               /* hottest cell temperature (°C) */
    uint64_t timestamp_ms;
};

/* Hard-rule trip thresholds. UL/IEC certified equivalent — these would
 * come from compliance docs in a real product. */
#define TRIP_V_OVER       4.25f   /* per-cell over-voltage */
#define TRIP_V_UNDER      2.50f   /* per-cell under-voltage */
#define TRIP_I_OVER       150.0f  /* pack over-current (|A|) */
#define TRIP_T_OVER       65.0f   /* over-temperature (°C) */

/* ML advisory thresholds — much tighter than hard rules. */
#define ADVISORY_V_DRIFT  0.05f   /* cell-to-cell voltage spread that triggers ML check */
#define ADVISORY_T_RATE   2.0f    /* °C/min: not yet a hard trip but worth watching */

/* Read/write the shared snapshot. */
void cell_state_set(const struct cell_snapshot *s);
void cell_state_get(struct cell_snapshot *out);

/* Trip / advisory state for inspection. */
struct status_flags {
    /* Layer 1: hard rule. If any of these is set, contactor should be open. */
    bool trip_ov;
    bool trip_uv;
    bool trip_oc;
    bool trip_ot;
    /* Layer 2: ML advisory. Logged but never opens the contactor on its own. */
    bool advisory_imbalance;
    bool advisory_thermal_trend;
    bool advisory_anomaly;
    /* counters */
    uint32_t safety_iters;
    uint32_t ml_iters;
    /* last measured ML "anomaly score" (synthetic for the demo) */
    float    last_anomaly_score;
};

void status_flags_get(struct status_flags *out);

#ifdef __cplusplus
}
#endif
#endif
