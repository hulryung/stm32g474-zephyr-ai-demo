/*
 * Layer 1: Safety thread.
 *
 * Highest priority, runs at 100 Hz, evaluates hard-rule trip conditions
 * only. Never makes a soft / probabilistic decision. Never blocks on the
 * ML side. If this thread's deadline is missed, watchdog should reset
 * the MCU.
 *
 * In production this thread would:
 *   - own the contactor GPIO
 *   - run from CCMRAM (deterministic, no flash-wait jitter)
 *   - be monitored by an independent task watchdog
 *   - have its own stack/heap (MPU isolated from ML region)
 */

#include "cell_state.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <math.h>

LOG_MODULE_REGISTER(safety, LOG_LEVEL_INF);

extern void status_flags_set(const struct status_flags *);

#define STACK_SIZE   1024
#define PRIORITY     2     /* very high; below kernel-internal threads only */

static K_THREAD_STACK_DEFINE(safety_stack, STACK_SIZE);
static struct k_thread safety_tid;

static void safety_loop(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct status_flags out = {0};
    while (1) {
        struct cell_snapshot s;
        cell_state_get(&s);

        struct status_flags prev;
        status_flags_get(&prev);
        out = prev;
        out.safety_iters++;

        /* Hard rules — every condition is a single comparison. Auditable.
         * NB: we OR rather than reset trips so the operator sees them
         * until the demo issues `safety reset`.
         */
        for (int c = 0; c < N_CELLS; ++c) {
            if (s.v[c] > TRIP_V_OVER && !out.trip_ov) {
                out.trip_ov = true;
                LOG_WRN("★ HARD TRIP: cell %d V=%.3f > %.3f (OV)",
                        c, (double)s.v[c], (double)TRIP_V_OVER);
            }
            if (s.v[c] < TRIP_V_UNDER && !out.trip_uv) {
                out.trip_uv = true;
                LOG_WRN("★ HARD TRIP: cell %d V=%.3f < %.3f (UV)",
                        c, (double)s.v[c], (double)TRIP_V_UNDER);
            }
        }
        if (fabsf(s.i_pack) > TRIP_I_OVER && !out.trip_oc) {
            out.trip_oc = true;
            LOG_WRN("★ HARD TRIP: I=%.1f > %.1f (OC)",
                    (double)s.i_pack, (double)TRIP_I_OVER);
        }
        if (s.t_max > TRIP_T_OVER && !out.trip_ot) {
            out.trip_ot = true;
            LOG_WRN("★ HARD TRIP: T=%.1f > %.1f (OT)",
                    (double)s.t_max, (double)TRIP_T_OVER);
        }
        status_flags_set(&out);

        /* Real product: open contactor if any trip; here we just log. */
        k_msleep(10);  /* 100 Hz */
    }
}

void safety_thread_start(void) {
    k_thread_create(&safety_tid, safety_stack, STACK_SIZE,
                    safety_loop, NULL, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&safety_tid, "safety");
}
