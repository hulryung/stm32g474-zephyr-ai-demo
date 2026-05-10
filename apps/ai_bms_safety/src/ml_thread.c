/*
 * Layer 2: ML advisory thread.
 *
 * Lower priority than safety. May be preempted, may even crash, must
 * never affect Layer 1's behavior. Evaluates trends and patterns the
 * hard rules can't catch:
 *   - cell-to-cell voltage imbalance (early warning of one bad cell)
 *   - thermal rise rate (catch cooling-system degradation before T_max trips)
 *   - anomaly score from the ai_bms autoencoder pattern (synthesized here
 *     since we don't carry the full TFLM stack into this demo)
 *
 * Outputs: `advisory_*` flags + a numeric anomaly score. NEVER opens
 * the contactor.
 */

#include "cell_state.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <math.h>

LOG_MODULE_REGISTER(ml, LOG_LEVEL_INF);

extern void status_flags_set(const struct status_flags *);

#define STACK_SIZE   2048
#define PRIORITY     10    /* well below safety; can be preempted */

static K_THREAD_STACK_DEFINE(ml_stack, STACK_SIZE);
static struct k_thread ml_tid;

static float prev_t_max = 25.0f;
static uint64_t prev_t_ms = 0;

static void ml_loop(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (1) {
        struct cell_snapshot s;
        cell_state_get(&s);

        struct status_flags out;
        status_flags_get(&out);
        out.ml_iters++;
        out.advisory_imbalance = false;
        out.advisory_thermal_trend = false;
        out.advisory_anomaly = false;

        /* --- Cell imbalance check --- */
        float vmax = s.v[0], vmin = s.v[0];
        for (int c = 1; c < N_CELLS; ++c) {
            if (s.v[c] > vmax) vmax = s.v[c];
            if (s.v[c] < vmin) vmin = s.v[c];
        }
        float spread = vmax - vmin;
        if (spread > ADVISORY_V_DRIFT) {
            out.advisory_imbalance = true;
        }

        /* --- Thermal rise rate (°C/min) --- */
        if (prev_t_ms > 0 && s.timestamp_ms > prev_t_ms) {
            float dt_min = (s.timestamp_ms - prev_t_ms) / 60000.0f;
            float dT = s.t_max - prev_t_max;
            if (dt_min > 0 && (dT / dt_min) > ADVISORY_T_RATE) {
                out.advisory_thermal_trend = true;
            }
        }
        prev_t_max = s.t_max;
        prev_t_ms = s.timestamp_ms;

        /* --- Synthetic "anomaly score" --- pretend this is the AE output.
         * In production, this thread would call the same TFLM glue we use
         * in apps/ai_bms. We just compute a simple stand-in: a weighted
         * sum of how far we are from "comfortable" operating envelope.
         */
        float score = 0.0f;
        score += spread * 4.0f;                                    /* imbalance */
        score += fmaxf(0.0f, s.t_max - 35.0f) * 0.05f;             /* warm operation */
        score += fmaxf(0.0f, fabsf(s.i_pack) - 50.0f) * 0.005f;    /* heavy current */
        for (int c = 0; c < N_CELLS; ++c) {
            score += fmaxf(0.0f, s.v[c] - 4.10f) * 2.0f;           /* near OV */
            score += fmaxf(0.0f, 2.80f - s.v[c]) * 2.0f;           /* near UV */
        }
        out.last_anomaly_score = score;
        if (score > 0.25f) {
            out.advisory_anomaly = true;
        }

        status_flags_set(&out);
        k_msleep(100);   /* 10 Hz — much slower than safety */
    }
}

void ml_thread_start(void) {
    k_thread_create(&ml_tid, ml_stack, STACK_SIZE,
                    ml_loop, NULL, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&ml_tid, "ml_advisory");
}
