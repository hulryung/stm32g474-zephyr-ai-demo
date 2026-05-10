/*
 * Shell tree: `persist ...`
 *
 *   persist info                        — show saved state + boot count
 *   persist save <soc> <Q> [cycles]     — save a synthetic state to NVS
 *   persist load                        — re-read NVS into RAM (simulate restart)
 *   persist clear                       — erase saved state (factory)
 *   persist demo                        — full key-off / key-on / OCV recalibration walk-through
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "bms_state.h"

/* OCV table — same as ai_bms_dual_ekf, used for rest-state recalibration. */
static const float OCV_SOC[21] = {
     0.00f,  5.00f, 10.00f, 15.00f, 20.00f, 25.00f, 30.00f,
    35.00f, 40.00f, 45.00f, 50.00f, 55.00f, 60.00f, 65.00f,
    70.00f, 75.00f, 80.00f, 85.00f, 90.00f, 95.00f, 100.00f,
};
static const float OCV_V[21] = {
    3.355f, 3.501f, 3.583f, 3.620f, 3.643f, 3.660f, 3.677f,
    3.694f, 3.712f, 3.732f, 3.755f, 3.781f, 3.812f, 3.847f,
    3.886f, 3.929f, 3.978f, 4.033f, 4.094f, 4.156f, 4.193f,
};

static float ocv_to_soc(float v) {
    if (v <= OCV_V[0])  return OCV_SOC[0];
    for (int i = 1; i < 21; ++i) {
        if (v <= OCV_V[i]) {
            float a = (v - OCV_V[i-1]) / (OCV_V[i] - OCV_V[i-1] + 1e-6f);
            return OCV_SOC[i-1] + a * (OCV_SOC[i] - OCV_SOC[i-1]);
        }
    }
    return OCV_SOC[20];
}

/* Demo recalibration policy: if the BMS has been off for > REST_THRESHOLD,
 * use the current OCV reading instead of (or blended with) the saved SOC.
 */
#define REST_THRESHOLD_SEC 600u   /* 10 minutes */

static int cmd_info(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    const struct bms_persisted_state *s = bms_state_get();
    if (!s) {
        shell_print(sh, "No persisted state. Boot count is 1 (this boot).");
        return 0;
    }
    shell_print(sh, "Persisted BMS state");
    shell_print(sh, "  schema version : %u", s->version);
    shell_print(sh, "  boot count     : %u", s->boot_count);
    shell_print(sh, "  SOC (saved)    : %.2f %%",  (double)s->soc_pct);
    shell_print(sh, "  Q estimate     : %.4f Ah", (double)s->Q_estimate_ah);
    shell_print(sh, "  V_RC           : %.4f V",  (double)s->v_rc);
    shell_print(sh, "  P_soc / P_Q    : %.4f / %.4f",
                (double)s->P_soc, (double)s->P_Q);
    shell_print(sh, "  cycles done    : %u", s->cycles_completed);
    shell_print(sh, "  uptime at save : %llu ms", s->shutdown_uptime_ms);
    return 0;
}

static int cmd_save(const struct shell *sh, size_t a, char **v) {
    if (a < 3 || a > 4) {
        shell_error(sh, "usage: persist save <soc%%> <Q_ah> [cycles_done]");
        return -EINVAL;
    }
    struct bms_persisted_state ns = {0};
    const struct bms_persisted_state *prev = bms_state_get();
    if (prev) ns = *prev;          /* keep boot_count, etc */
    ns.version = BMS_STATE_VERSION;
    ns.soc_pct = strtof(v[1], NULL);
    ns.Q_estimate_ah = strtof(v[2], NULL);
    if (a == 4) ns.cycles_completed = (uint32_t)strtoul(v[3], NULL, 10);
    ns.shutdown_uptime_ms = k_uptime_get();
    /* For demo we don't have an RTC — caller can simulate "key-off" duration
     * via `persist load <rest_seconds>`. */
    ns.shutdown_wallclock_s = 0;
    int rc = bms_state_set(&ns);
    if (rc) { shell_error(sh, "save failed: %d", rc); return rc; }
    shell_print(sh, "saved: SOC=%.2f%% Q=%.4fAh", (double)ns.soc_pct, (double)ns.Q_estimate_ah);
    return 0;
}

static int cmd_clear(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    int rc = bms_state_clear();
    shell_print(sh, "cleared (rc=%d). Reboot or `persist load` to confirm.", rc);
    return 0;
}

static int cmd_load(const struct shell *sh, size_t a, char **v) {
    /* Simulate "key-on after rest_seconds rest". Pass simulated current
     * cell V to demonstrate the OCV recalibration policy.
     *  usage: persist load <rest_sec> [v_now]
     */
    if (a < 2 || a > 3) {
        shell_error(sh, "usage: persist load <rest_seconds> [V_now_for_OCV]");
        return -EINVAL;
    }
    uint32_t rest_s = (uint32_t)strtoul(v[1], NULL, 10);
    bool have_v = (a == 3);
    float v_now = have_v ? strtof(v[2], NULL) : 0.0f;

    const struct bms_persisted_state *s = bms_state_get();
    shell_print(sh, "Simulating key-on after %u s rest:", rest_s);
    if (!s) {
        shell_print(sh, "  no saved state — fresh boot, SOC unknown");
        if (have_v) {
            float ocv_soc = ocv_to_soc(v_now);
            shell_print(sh, "  using OCV reading %.4f V → SOC %.2f %%",
                        (double)v_now, (double)ocv_soc);
        } else {
            shell_print(sh, "  no OCV reading available — defaulting to 50 %%");
        }
        return 0;
    }
    shell_print(sh, "  saved SOC = %.2f %%, saved Q = %.4f Ah",
                (double)s->soc_pct, (double)s->Q_estimate_ah);

    if (rest_s < REST_THRESHOLD_SEC) {
        shell_print(sh, "  rest %u s < threshold %u s → trust saved SOC",
                    rest_s, (unsigned)REST_THRESHOLD_SEC);
        shell_print(sh, "  active SOC = %.2f %%", (double)s->soc_pct);
    } else if (have_v) {
        float ocv_soc = ocv_to_soc(v_now);
        float blended = 0.3f * s->soc_pct + 0.7f * ocv_soc;
        shell_print(sh, "  rest %u s ≥ threshold %u s → recalibrate from OCV",
                    rest_s, (unsigned)REST_THRESHOLD_SEC);
        shell_print(sh, "  V terminal at rest = %.4f V → OCV-derived SOC = %.2f %%",
                    (double)v_now, (double)ocv_soc);
        shell_print(sh, "  blended SOC (0.3*saved + 0.7*OCV) = %.2f %%",
                    (double)blended);
        shell_print(sh, "  saved-vs-OCV diff = %+.2f %% (drift detector)",
                    (double)(s->soc_pct - ocv_soc));
    } else {
        shell_print(sh, "  rest long enough for OCV but no V given — keeping saved SOC");
    }
    return 0;
}

static int cmd_demo(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    shell_print(sh, "");
    shell_print(sh, "════════════════════════════════════════════════════════");
    shell_print(sh, " Persistence + OCV recalibration demo");
    shell_print(sh, "════════════════════════════════════════════════════════");
    shell_print(sh, "");
    shell_print(sh, "Step 1: clear any prior state");
    bms_state_clear();
    shell_print(sh, "  → cleared.");
    shell_print(sh, "");
    shell_print(sh, "Step 2: simulate normal operation, save SOC=42%%, Q=1.78Ah");
    struct bms_persisted_state s = {0};
    s.soc_pct = 42.0f;
    s.Q_estimate_ah = 1.78f;
    s.v_rc = 0.012f;
    s.P_soc = 0.05f;
    s.P_Q = 0.001f;
    s.cycles_completed = 137;
    s.shutdown_uptime_ms = k_uptime_get();
    bms_state_set(&s);
    shell_print(sh, "  → saved.");
    shell_print(sh, "");
    shell_print(sh, "Step 3: simulate quick key-cycle (60s rest, no OCV needed)");
    shell_print(sh, "  → trust saved SOC");
    shell_print(sh, "  active SOC = 42.00 %% (unchanged)");
    shell_print(sh, "");
    shell_print(sh, "Step 4: simulate long rest (1 hour) + V_terminal = 3.732V");
    float ocv_soc = ocv_to_soc(3.732f);
    shell_print(sh, "  rest > threshold → use OCV");
    shell_print(sh, "  OCV-derived SOC = %.2f %%", (double)ocv_soc);
    shell_print(sh, "  saved SOC = 42.00 %%");
    shell_print(sh, "  drift = %+.2f %%", (double)(42.0f - ocv_soc));
    shell_print(sh, "  blended (0.3 saved + 0.7 OCV) = %.2f %%",
                (double)(0.3f * 42.0f + 0.7f * ocv_soc));
    shell_print(sh, "");
    shell_print(sh, "Step 5: simulate factory reset");
    bms_state_clear();
    shell_print(sh, "  → cleared. Next boot starts from scratch.");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_persist,
    SHELL_CMD(info,  NULL, "Show saved BMS state",            cmd_info),
    SHELL_CMD_ARG(save,  NULL, "save <soc%> <Q_ah> [cycles]",
                  cmd_save, 3, 1),
    SHELL_CMD_ARG(load,  NULL, "load <rest_seconds> [V_now]",
                  cmd_load, 2, 1),
    SHELL_CMD(clear, NULL, "Erase saved state",                cmd_clear),
    SHELL_CMD(demo,  NULL, "Walk through the recalibration policy", cmd_demo),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(persist, &sub_persist,
                   "BMS state persistence + OCV recalibration", NULL);
