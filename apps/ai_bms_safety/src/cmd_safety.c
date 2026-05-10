/*
 * Shell tree: `cell` (input) + `safety` (output)
 *
 *   cell sim <c0> <c1> <c2> <c3> <i> <t_max>   — set the simulated snapshot
 *   cell show                                    — read back the snapshot
 *
 *   safety status                                — flags from both threads
 *   safety reset                                 — clear all trip flags
 *   safety scenario <n>                          — run a canned scenario
 *
 * Available scenarios:
 *   1 = healthy operation
 *   2 = mild cell imbalance (advisory only, no hard trip)
 *   3 = thermal rise (advisory only)
 *   4 = single-cell over-voltage (HARD TRIP)
 *   5 = pack over-current (HARD TRIP)
 *   6 = combined trend + trip (advisory fires AHEAD of hard trip)
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>

#include "cell_state.h"

extern void status_flags_set(const struct status_flags *);

static int cmd_sim(const struct shell *sh, size_t a, char **v) {
    if (a != 7) {
        shell_error(sh, "usage: cell sim <c0> <c1> <c2> <c3> <i> <t_max>");
        return -EINVAL;
    }
    struct cell_snapshot s;
    s.v[0] = strtof(v[1], NULL);
    s.v[1] = strtof(v[2], NULL);
    s.v[2] = strtof(v[3], NULL);
    s.v[3] = strtof(v[4], NULL);
    s.i_pack = strtof(v[5], NULL);
    s.t_max = strtof(v[6], NULL);
    s.timestamp_ms = k_uptime_get();
    cell_state_set(&s);
    shell_print(sh, "snapshot set");
    return 0;
}

static int cmd_show(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    struct cell_snapshot s;
    cell_state_get(&s);
    shell_print(sh, "Cell V: %.3f %.3f %.3f %.3f V",
                (double)s.v[0], (double)s.v[1], (double)s.v[2], (double)s.v[3]);
    shell_print(sh, "Pack I: %.2f A", (double)s.i_pack);
    shell_print(sh, "T_max : %.1f °C", (double)s.t_max);
    return 0;
}

static int cmd_status(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    struct status_flags f;
    status_flags_get(&f);
    shell_print(sh, "Layer 1 (HARD RULES — would open contactor):");
    shell_print(sh, "  trip_ov : %s", f.trip_ov ? "YES ★" : "no");
    shell_print(sh, "  trip_uv : %s", f.trip_uv ? "YES ★" : "no");
    shell_print(sh, "  trip_oc : %s", f.trip_oc ? "YES ★" : "no");
    shell_print(sh, "  trip_ot : %s", f.trip_ot ? "YES ★" : "no");
    shell_print(sh, "");
    shell_print(sh, "Layer 2 (ML ADVISORY — log only, no contactor action):");
    shell_print(sh, "  advisory_imbalance      : %s", f.advisory_imbalance ? "YES" : "no");
    shell_print(sh, "  advisory_thermal_trend  : %s", f.advisory_thermal_trend ? "YES" : "no");
    shell_print(sh, "  advisory_anomaly        : %s", f.advisory_anomaly ? "YES" : "no");
    shell_print(sh, "  anomaly_score           : %.4f", (double)f.last_anomaly_score);
    shell_print(sh, "");
    shell_print(sh, "Iters: safety=%u, ml=%u  (separation = %.0f×)",
                f.safety_iters, f.ml_iters,
                (double)((f.ml_iters > 0) ? (float)f.safety_iters / f.ml_iters : 0.0f));
    return 0;
}

static int cmd_reset(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    struct status_flags f = {0};
    status_flags_set(&f);
    shell_print(sh, "all flags + counters cleared");
    return 0;
}

static int cmd_scenario(const struct shell *sh, size_t a, char **v) {
    if (a != 2) { shell_error(sh, "usage: safety scenario <1..6>"); return -EINVAL; }
    int n = atoi(v[1]);
    struct cell_snapshot s = {0};
    s.timestamp_ms = k_uptime_get();
    const char *desc = "?";

    switch (n) {
    case 1:
        s.v[0] = 3.85f; s.v[1] = 3.86f; s.v[2] = 3.85f; s.v[3] = 3.84f;
        s.i_pack = -1.5f; s.t_max = 28.0f;
        desc = "healthy 50%% SOC discharge";
        break;
    case 2:
        s.v[0] = 3.85f; s.v[1] = 3.84f; s.v[2] = 3.85f; s.v[3] = 3.78f;  /* cell 3 sagging */
        s.i_pack = -1.5f; s.t_max = 30.0f;
        desc = "mild cell imbalance (cell 3 sagging) — advisory only";
        break;
    case 3:
        s.v[0] = 3.95f; s.v[1] = 3.95f; s.v[2] = 3.95f; s.v[3] = 3.94f;
        s.i_pack = -8.0f; s.t_max = 48.0f;
        desc = "thermal rising under heavy current — advisory only";
        break;
    case 4:
        s.v[0] = 3.85f; s.v[1] = 3.85f; s.v[2] = 4.31f; s.v[3] = 3.85f;  /* cell 2 OV */
        s.i_pack = +1.0f; s.t_max = 35.0f;
        desc = "cell 2 over-voltage 4.31 V — HARD TRIP expected";
        break;
    case 5:
        s.v[0] = 3.50f; s.v[1] = 3.51f; s.v[2] = 3.51f; s.v[3] = 3.50f;
        s.i_pack = -180.0f; s.t_max = 40.0f;                              /* huge discharge */
        desc = "pack over-current 180 A — HARD TRIP expected";
        break;
    case 6:
        s.v[0] = 4.18f; s.v[1] = 4.20f; s.v[2] = 4.19f; s.v[3] = 4.06f;  /* mild imbalance */
        s.i_pack = -3.0f; s.t_max = 52.0f;
        desc = "trend rising — ML advisory but no hard trip yet";
        break;
    default:
        shell_error(sh, "scenario must be 1..6");
        return -EINVAL;
    }
    cell_state_set(&s);
    shell_print(sh, "Scenario %d: %s", n, desc);
    shell_print(sh, "Wait ~200 ms for both threads to evaluate, then `safety status`");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cell,
    SHELL_CMD_ARG(sim,  NULL, "cell sim <c0..c3> <i> <t_max>",
                  cmd_sim, 7, 0),
    SHELL_CMD(show, NULL, "Read current cell snapshot", cmd_show),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(cell, &sub_cell, "Simulated cell front-end", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_safety,
    SHELL_CMD(status, NULL, "Show trip + advisory flags", cmd_status),
    SHELL_CMD(reset,  NULL, "Clear all flags + counters", cmd_reset),
    SHELL_CMD_ARG(scenario, NULL, "safety scenario <1..6>",
                  cmd_scenario, 2, 0),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(safety, &sub_safety,
                   "Layer 1/2 BMS safety architecture demo", NULL);
