/*
 * Shell command tree: `soh ...`
 *
 *   soh list                 — list embedded sample cycles
 *   soh info                 — model info
 *   soh estimate <name>      — predict capacity for one cycle
 *   soh scan                 — predict for every embedded cycle, print error
 *   soh bench [iterations]   — inference latency
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tflm_soh.h"
#include "cycles_db.h"

static const struct soh_cycle *find_cycle(const char *name)
{
	for (unsigned int i = 0; i < g_cycle_count; ++i) {
		if (!strcmp(g_cycles[i].name, name)) {
			return &g_cycles[i];
		}
	}
	return NULL;
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "%-7s %-7s %-5s %s",
		    "name", "cell", "idx", "true capacity");
	for (unsigned int i = 0; i < g_cycle_count; ++i) {
		const struct soh_cycle *c = &g_cycles[i];
		shell_print(sh, "%-7s %-7s %-5u %.4f Ah",
			    c->name, c->cell, c->index,
			    (double)c->true_capacity_ah);
	}
	return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "Model    : MLP regressor (int8)");
	shell_print(sh, "Trained  : NASA PCoE B0005/B0006/B0007 healthy + aged cycles");
	shell_print(sh, "Holdout  : B0018 (embedded cycles all from this cell)");
	shell_print(sh, "Cycles   : %u embedded", g_cycle_count);
	shell_print(sh, "Arena    : %u bytes", tflm_soh_arena_size());
	shell_print(sh, "Output   : capacity in Ah, range [1.0, 2.0]");
	return 0;
}

static int estimate_cycle(const struct shell *sh, const struct soh_cycle *c)
{
	float pred;
	int rc = tflm_soh_estimate(c->voltage_norm, &pred);
	if (rc != 0) {
		shell_error(sh, "inference failed: %d", rc);
		return rc;
	}
	float err = pred - c->true_capacity_ah;
	float pct = (c->true_capacity_ah > 0.0f) ?
		    (fabsf(err) / c->true_capacity_ah * 100.0f) : 0.0f;
	shell_print(sh, "%-7s true=%.4f Ah  predicted=%.4f Ah  error=%+.4f Ah (%.1f%%)",
		    c->name,
		    (double)c->true_capacity_ah,
		    (double)pred,
		    (double)err,
		    (double)pct);
	return 0;
}

static int cmd_estimate(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "usage: soh estimate <cycle_name>");
		return -EINVAL;
	}
	const struct soh_cycle *c = find_cycle(argv[1]);
	if (!c) {
		shell_error(sh, "unknown cycle '%s' (try `soh list`)", argv[1]);
		return -EINVAL;
	}
	return estimate_cycle(sh, c);
}

static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "SOH estimation across all embedded cycles:");
	float total_abs_err = 0.0f;
	for (unsigned int i = 0; i < g_cycle_count; ++i) {
		float pred;
		if (tflm_soh_estimate(g_cycles[i].voltage_norm, &pred) != 0) {
			continue;
		}
		float err = pred - g_cycles[i].true_capacity_ah;
		total_abs_err += fabsf(err);
		estimate_cycle(sh, &g_cycles[i]);
	}
	if (g_cycle_count > 0) {
		shell_print(sh, "MAE: %.4f Ah (%u cycles)",
			    (double)(total_abs_err / g_cycle_count), g_cycle_count);
	}
	return 0;
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = 1000;
	if (argc == 2) { n = (uint32_t)strtoul(argv[1], NULL, 10); if (!n) n = 1000; }
	if (g_cycle_count == 0) { return -ENODATA; }
	const float *window = g_cycles[0].voltage_norm;
	float pred;

	for (int i = 0; i < 4; ++i) tflm_soh_estimate(window, &pred);

	uint32_t cyc_per_us = sys_clock_hw_cycles_per_sec() / 1000000U;
	uint32_t mn = UINT32_MAX, mx = 0;
	uint64_t total = 0;
	for (uint32_t i = 0; i < n; ++i) {
		uint32_t t0 = k_cycle_get_32();
		tflm_soh_estimate(window, &pred);
		uint32_t dt = k_cycle_get_32() - t0;
		total += dt;
		if (dt < mn) mn = dt;
		if (dt > mx) mx = dt;
	}
	uint32_t avg = (uint32_t)(total / n);
	shell_print(sh, "iterations : %u", n);
	shell_print(sh, "avg        : %u cycles  (%u.%03u us)",
		    avg, avg / cyc_per_us, (avg * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "min        : %u cycles  (%u.%03u us)",
		    mn, mn / cyc_per_us, (mn * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "max        : %u cycles  (%u.%03u us)",
		    mx, mx / cyc_per_us, (mx * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "throughput : ~%u inf/sec",
		    (uint32_t)(sys_clock_hw_cycles_per_sec() / avg));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_soh,
	SHELL_CMD(list,    NULL, "List embedded cycles + true capacity",   cmd_list),
	SHELL_CMD(info,    NULL, "Model info",                              cmd_info),
	SHELL_CMD_ARG(estimate, NULL, "soh estimate <cycle_name>",
		      cmd_estimate, 2, 0),
	SHELL_CMD(scan,    NULL, "Estimate every embedded cycle + MAE",     cmd_scan),
	SHELL_CMD_ARG(bench, NULL, "Inference latency [iterations]",
		      cmd_bench, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(soh, &sub_soh,
		   "Battery SOH (state of health) regression demo", NULL);
