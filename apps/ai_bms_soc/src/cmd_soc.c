/*
 * Shell command tree: `soc ...`
 *
 *   soc list                          — list embedded sample windows
 *   soc info                          — model + EKF param info
 *   soc compare <name>                — run all 6 estimators on one sample, table
 *   soc cycle <name> <method>         — single estimator on one sample
 *   soc bench <method> [iterations]   — latency for one estimator
 *   soc benchall                      — bench all 6 on the same sample
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "soc_estimators.h"

static const struct soc_sample *find_sample(const char *name)
{
    for (unsigned i = 0; i < g_sample_count; ++i) {
        if (!strcmp(g_samples[i].name, name)) return &g_samples[i];
    }
    return NULL;
}

/* Aggregate all 6 estimators on one sample, return SOC for each */
struct estimates {
    float cc, ocv, ekf, mlp, lstm, hybrid;
};

static void run_all(const struct soc_sample *s, struct estimates *e)
{
    e->cc     = soc_cc_window(s, /*soc0=*/100.0f);
    /* OCV: use the last sample's voltage (not really fair — voltage is
     * depressed by IR drop during discharge — but that's the point. */
    e->ocv    = soc_ocv_lookup(s->v[SOC_WINDOW-1]);
    e->ekf    = soc_ekf_window(s, 100.0f);
    e->mlp    = soc_mlp_window(s);
    e->lstm   = soc_lstm_window(s);
    e->hybrid = soc_hybrid_window(s, 100.0f);
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "%-7s %-7s %-9s %-9s %-9s",
                "name", "cell", "cap (Ah)", "SOC start", "SOC end");
    for (unsigned i = 0; i < g_sample_count; ++i) {
        const struct soc_sample *s = &g_samples[i];
        shell_print(sh, "%-7s %-7s %.4f    %.2f      %.2f",
                    s->name, s->cell, (double)s->capacity_ah,
                    (double)s->soc_true[0],
                    (double)s->soc_true[SOC_WINDOW-1]);
    }
    return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "Embedded test cycles : %u (from B0018 holdout cell)", g_sample_count);
    shell_print(sh, "Window length        : %d samples", SOC_WINDOW);
    shell_print(sh, "");
    shell_print(sh, "Classical SOC stack:");
    shell_print(sh, "  ECM R0   = %.4f ohms", (double)g_ecm_R0);
    shell_print(sh, "  ECM R1   = %.4f ohms", (double)g_ecm_R1);
    shell_print(sh, "  ECM C1   = %.1f F  (tau=%.1fs)",
                (double)g_ecm_C1, (double)(g_ecm_R1*g_ecm_C1));
    shell_print(sh, "  Q nom    = %.4f Ah", (double)g_ecm_Qnom);
    shell_print(sh, "  OCV bins = %u (range %.3f..%.3f V)",
                g_sococv_n,
                (double)g_sococv_ocv[0],
                (double)g_sococv_ocv[g_sococv_n-1]);
    shell_print(sh, "");
    shell_print(sh, "ML stack:");
    shell_print(sh, "  MLP    arena = %u B", soc_mlp_arena_size());
    shell_print(sh, "  LSTM   arena = %u B", soc_lstm_arena_size());
    shell_print(sh, "  Hybrid arena = %u B", soc_hybrid_arena_size());
    return 0;
}

static void print_row(const struct shell *sh, const char *method,
                      float pred, float truth)
{
    float err = pred - truth;
    shell_print(sh, "  %-22s %7.2f %%   %+7.2f %%",
                method, (double)pred, (double)err);
}

static int cmd_compare(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "usage: soc compare <name>");
        return -EINVAL;
    }
    const struct soc_sample *s = find_sample(argv[1]);
    if (!s) {
        shell_error(sh, "unknown sample '%s' (try `soc list`)", argv[1]);
        return -EINVAL;
    }
    float truth = s->soc_true[SOC_WINDOW-1];
    struct estimates e;
    run_all(s, &e);
    shell_print(sh, "");
    shell_print(sh, "Sample: %s (%s)  capacity=%.4f Ah",
                s->name, s->cell, (double)s->capacity_ah);
    shell_print(sh, "True end-of-window SOC: %.2f %%", (double)truth);
    shell_print(sh, "");
    shell_print(sh, "  %-22s %-9s %-9s",
                "method", "predicted", "error");
    shell_print(sh, "  ──────────────────────────────────────────────────");
    print_row(sh, "1. coulomb counting",   e.cc,     truth);
    print_row(sh, "2. OCV lookup",          e.ocv,    truth);
    print_row(sh, "3. EKF (3-state ECM)",   e.ekf,    truth);
    print_row(sh, "4. MLP (per-sample)",    e.mlp,    truth);
    print_row(sh, "5. LSTM (sequence)",     e.lstm,   truth);
    print_row(sh, "6. Hybrid EKF + MLP",    e.hybrid, truth);
    return 0;
}

static int cmd_cycle(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "usage: soc cycle <name> <method>");
        shell_error(sh, "       method: cc | ocv | ekf | mlp | lstm | hybrid");
        return -EINVAL;
    }
    const struct soc_sample *s = find_sample(argv[1]);
    if (!s) { shell_error(sh, "unknown sample '%s'", argv[1]); return -EINVAL; }
    float pred = NAN;
    if      (!strcmp(argv[2], "cc"))     pred = soc_cc_window(s, 100.0f);
    else if (!strcmp(argv[2], "ocv"))    pred = soc_ocv_lookup(s->v[SOC_WINDOW-1]);
    else if (!strcmp(argv[2], "ekf"))    pred = soc_ekf_window(s, 100.0f);
    else if (!strcmp(argv[2], "mlp"))    pred = soc_mlp_window(s);
    else if (!strcmp(argv[2], "lstm"))   pred = soc_lstm_window(s);
    else if (!strcmp(argv[2], "hybrid")) pred = soc_hybrid_window(s, 100.0f);
    else { shell_error(sh, "unknown method '%s'", argv[2]); return -EINVAL; }
    float truth = s->soc_true[SOC_WINDOW-1];
    shell_print(sh, "%-7s %-7s  predicted=%.2f %%  true=%.2f %%  error=%+.2f %%",
                s->name, argv[2],
                (double)pred, (double)truth, (double)(pred - truth));
    return 0;
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        shell_error(sh, "usage: soc bench <method> [iterations]");
        return -EINVAL;
    }
    uint32_t n = (argc == 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 1000U;
    if (!n) n = 1000;
    const struct soc_sample *s = &g_samples[0];

    /* Pre-warm */
    for (int i = 0; i < 4; ++i) {
        if      (!strcmp(argv[1], "cc"))     soc_cc_window(s, 100.0f);
        else if (!strcmp(argv[1], "ocv"))    soc_ocv_lookup(s->v[0]);
        else if (!strcmp(argv[1], "ekf"))    soc_ekf_window(s, 100.0f);
        else if (!strcmp(argv[1], "mlp"))    soc_mlp_window(s);
        else if (!strcmp(argv[1], "lstm"))   soc_lstm_window(s);
        else if (!strcmp(argv[1], "hybrid")) soc_hybrid_window(s, 100.0f);
        else { shell_error(sh, "unknown method '%s'", argv[1]); return -EINVAL; }
    }

    uint32_t cyc_per_us = sys_clock_hw_cycles_per_sec() / 1000000U;
    uint32_t mn = UINT32_MAX, mx = 0;
    uint64_t total = 0;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t t0 = k_cycle_get_32();
        if      (!strcmp(argv[1], "cc"))     soc_cc_window(s, 100.0f);
        else if (!strcmp(argv[1], "ocv"))    soc_ocv_lookup(s->v[0]);
        else if (!strcmp(argv[1], "ekf"))    soc_ekf_window(s, 100.0f);
        else if (!strcmp(argv[1], "mlp"))    soc_mlp_window(s);
        else if (!strcmp(argv[1], "lstm"))   soc_lstm_window(s);
        else                                  soc_hybrid_window(s, 100.0f);
        uint32_t dt = k_cycle_get_32() - t0;
        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }
    uint32_t avg = (uint32_t)(total / n);
    shell_print(sh, "%s: avg=%u cyc (%u.%03u us)  min=%u  max=%u  ~%u/sec",
                argv[1],
                avg, avg / cyc_per_us, (avg * 1000U / cyc_per_us) % 1000U,
                mn, mx,
                (uint32_t)(sys_clock_hw_cycles_per_sec() / avg));
    return 0;
}

static int cmd_benchall(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    const char *methods[] = {"cc", "ocv", "ekf", "mlp", "lstm", "hybrid"};
    for (size_t i = 0; i < ARRAY_SIZE(methods); ++i) {
        char arg1[16];
        char arg2[8];
        strncpy(arg1, methods[i], sizeof(arg1));
        strncpy(arg2, "200", sizeof(arg2));
        char *argv2[] = {(char*)"bench", arg1, arg2};
        cmd_bench(sh, 3, argv2);
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_soc,
    SHELL_CMD(list,    NULL, "List embedded test windows",   cmd_list),
    SHELL_CMD(info,    NULL, "EKF + ML stack info",          cmd_info),
    SHELL_CMD_ARG(compare, NULL, "soc compare <name> — run all 6 estimators",
                  cmd_compare, 2, 0),
    SHELL_CMD_ARG(cycle, NULL, "soc cycle <name> <method>",
                  cmd_cycle, 3, 0),
    SHELL_CMD_ARG(bench, NULL, "soc bench <method> [iterations]",
                  cmd_bench, 2, 1),
    SHELL_CMD(benchall, NULL, "Bench all 6 estimators",      cmd_benchall),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(soc, &sub_soc,
                   "SOC estimation — 6 techniques head-to-head", NULL);
