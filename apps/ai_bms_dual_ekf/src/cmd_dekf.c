/*
 * Shell command tree: `dekf ...`
 *
 *   dekf info               — current SOC, Q estimate, SOH, P matrices
 *   dekf reset              — reset both EKFs to fresh-cell defaults
 *   dekf run                — replay all 84 NASA B0005 cycles in order, log Q
 *   dekf cycle <n>          — run only cycle n
 *   dekf table              — print Q_actual vs Q_estimate per cycle (scrolls)
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dual_ekf.h"
#include "cycles_db.h"

/* ECM params (literature defaults — copied from ai_bms_soc) */
#define ECM_R0  0.060f
#define ECM_R1  0.025f
#define ECM_C1  2000.0f
#define Q_NOMINAL 1.85f          /* B0005 nominal at start */

static struct dual_ekf g_dekf;
static bool            g_initialized = false;

static void ensure_init(void) {
    if (!g_initialized) {
        dual_ekf_init(&g_dekf, /*soc0=*/100.0f, /*Q=*/Q_NOMINAL,
                      ECM_R0, ECM_R1, ECM_C1);
        g_initialized = true;
    }
}

/* Run one stored cycle through the dual EKF. Uses the cycle's recorded
 * total duration to compute realistic per-step dt for the integrator.
 */
static void run_cycle(const struct cycle_record *c) {
    float dt = c->duration_s / (float)CYCLE_WINDOW;
    for (int k = 0; k < CYCLE_WINDOW; ++k) {
        dual_ekf_step(&g_dekf, c->v[k], c->i[k], dt);
    }
    dual_ekf_end_of_cycle(&g_dekf, /*ended_at=*/0.0f);
    /* Reset SOC for next charge — real BMS sees a charge cycle in between
     * but our test data is discharge-only, so just snap back to 100 %. */
    g_dekf.soc_pct = 100.0f;
    g_dekf.soc_at_cycle_start = 100.0f;
    g_dekf.v_rc = 0.0f;
}

/* ---- commands ---------------------------------------------------------- */

static int cmd_info(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    ensure_init();
    shell_print(sh, "Cell:                B0005 (NASA PCoE)");
    shell_print(sh, "Cycles in database:  %u (every-other from 168)", g_cycle_count);
    shell_print(sh, "Nominal capacity Q0: %.4f Ah", (double)Q_NOMINAL);
    shell_print(sh, "");
    shell_print(sh, "Current dual-EKF state:");
    shell_print(sh, "  SOC          : %.2f %%", (double)dual_ekf_soc(&g_dekf));
    shell_print(sh, "  Q estimate   : %.4f Ah", (double)dual_ekf_capacity(&g_dekf));
    shell_print(sh, "  SOH          : %.1f %% of nominal",
                (double)dual_ekf_soh_pct(&g_dekf, Q_NOMINAL));
    shell_print(sh, "  P_soc        : %.4f", (double)g_dekf.P_soc);
    shell_print(sh, "  P_Q          : %.4f", (double)g_dekf.P_Q);
    return 0;
}

static int cmd_reset(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    dual_ekf_init(&g_dekf, 100.0f, Q_NOMINAL, ECM_R0, ECM_R1, ECM_C1);
    g_initialized = true;
    shell_print(sh, "dual EKF reset to fresh cell (Q=%.4f Ah, SOC=100%%)",
                (double)Q_NOMINAL);
    return 0;
}

static int cmd_run(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    ensure_init();
    shell_print(sh, "Replaying %u cycles…", g_cycle_count);
    shell_print(sh, "  %-6s %-12s %-12s %-12s %s",
                "cycle", "Q_actual", "Q_estim", "error", "SOH");
    for (unsigned i = 0; i < g_cycle_count; ++i) {
        run_cycle(&g_cycles[i]);
        if (i < 5 || i % 10 == 0 || i == g_cycle_count - 1) {
            float qe = dual_ekf_capacity(&g_dekf);
            float qa = g_cycles[i].Q_actual;
            shell_print(sh, "  %-6u %-12.4f %-12.4f %+-12.4f %.1f%%",
                        g_cycles[i].index,
                        (double)qa, (double)qe,
                        (double)(qe - qa),
                        (double)dual_ekf_soh_pct(&g_dekf, Q_NOMINAL));
        }
    }
    shell_print(sh, "");
    shell_print(sh, "Final: Q_estim=%.4f Ah  Q_actual=%.4f Ah  err=%+.4f Ah",
                (double)dual_ekf_capacity(&g_dekf),
                (double)g_cycles[g_cycle_count-1].Q_actual,
                (double)(dual_ekf_capacity(&g_dekf) -
                         g_cycles[g_cycle_count-1].Q_actual));
    return 0;
}

static int cmd_cycle(const struct shell *sh, size_t a, char **v) {
    if (a != 2) { shell_error(sh, "usage: dekf cycle <n>"); return -EINVAL; }
    unsigned n = (unsigned)strtoul(v[1], NULL, 10);
    if (n >= g_cycle_count) {
        shell_error(sh, "cycle %u out of range (0..%u)", n, g_cycle_count-1);
        return -EINVAL;
    }
    ensure_init();
    run_cycle(&g_cycles[n]);
    shell_print(sh, "After cycle %u (real index %u, Q_actual=%.4f Ah):",
                n, g_cycles[n].index, (double)g_cycles[n].Q_actual);
    shell_print(sh, "  Q_estimate = %.4f Ah  (err=%+.4f Ah)",
                (double)dual_ekf_capacity(&g_dekf),
                (double)(dual_ekf_capacity(&g_dekf) - g_cycles[n].Q_actual));
    shell_print(sh, "  SOH        = %.1f %%",
                (double)dual_ekf_soh_pct(&g_dekf, Q_NOMINAL));
    return 0;
}

static int cmd_table(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    /* Re-run from fresh cell, log every cycle (warning: ~84 lines). */
    dual_ekf_init(&g_dekf, 100.0f, Q_NOMINAL, ECM_R0, ECM_R1, ECM_C1);
    shell_print(sh, "  %-6s %-12s %-12s %-12s",
                "cycle", "Q_actual", "Q_estim", "error");
    for (unsigned i = 0; i < g_cycle_count; ++i) {
        run_cycle(&g_cycles[i]);
        shell_print(sh, "  %-6u %-12.4f %-12.4f %+-12.4f",
                    g_cycles[i].index,
                    (double)g_cycles[i].Q_actual,
                    (double)dual_ekf_capacity(&g_dekf),
                    (double)(dual_ekf_capacity(&g_dekf) - g_cycles[i].Q_actual));
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_dekf,
    SHELL_CMD(info,  NULL, "Show current dual-EKF state",       cmd_info),
    SHELL_CMD(reset, NULL, "Reset to fresh cell defaults",      cmd_reset),
    SHELL_CMD(run,   NULL, "Replay all cycles, summary log",    cmd_run),
    SHELL_CMD_ARG(cycle, NULL, "Run one cycle: dekf cycle <n>", cmd_cycle, 2, 0),
    SHELL_CMD(table, NULL, "Per-cycle Q_actual vs Q_estimate",  cmd_table),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(dekf, &sub_dekf,
                   "Dual EKF — SOC + capacity tracking demo", NULL);
