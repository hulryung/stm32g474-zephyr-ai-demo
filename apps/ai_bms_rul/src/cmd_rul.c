/*
 * Shell command tree: `rul ...`
 *
 *   rul list                — list embedded cycles + true RUL
 *   rul info                — model info
 *   rul predict <name>      — predict RUL for one cycle
 *   rul scan                — predict for all + show errors
 *   rul bench [iterations]  — inference latency
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>

#include "tflm_rul.h"
#include "cycles_db.h"

static const struct rul_cycle *find_cycle(const char *name)
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
	shell_print(sh, "%-7s %-7s %-5s %-10s %s",
		    "name", "cell", "idx", "capacity", "true RUL");
	for (unsigned int i = 0; i < g_cycle_count; ++i) {
		const struct rul_cycle *c = &g_cycles[i];
		shell_print(sh, "%-7s %-7s %-5u %.4f Ah  %u cycles",
			    c->name, c->cell, c->index,
			    (double)c->capacity_ah, c->true_rul);
	}
	return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "Model    : MLP regressor (int8)");
	shell_print(sh, "Trained  : NASA PCoE B0005/B0006/B0007 — RUL labeled");
	shell_print(sh, "EOL def  : capacity < 1.5 Ah");
	shell_print(sh, "Holdout  : B0018 — embedded cycles all from this cell");
	shell_print(sh, "Cycles   : %u embedded", g_cycle_count);
	shell_print(sh, "Arena    : %u bytes", tflm_rul_arena_size());
	shell_print(sh, "Output   : remaining cycles, range [0, 150]");
	return 0;
}

static int predict_cycle(const struct shell *sh, const struct rul_cycle *c)
{
	int pred;
	if (tflm_rul_predict(c->voltage_norm, &pred) != 0) {
		shell_error(sh, "inference failed");
		return -EIO;
	}
	int err = pred - (int)c->true_rul;
	shell_print(sh, "%-7s cap=%.3f Ah  true_RUL=%-3u  predicted_RUL=%-3d  error=%+d cycles",
		    c->name, (double)c->capacity_ah, c->true_rul, pred, err);
	return 0;
}

static int cmd_predict(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "usage: rul predict <cycle_name>");
		return -EINVAL;
	}
	const struct rul_cycle *c = find_cycle(argv[1]);
	if (!c) {
		shell_error(sh, "unknown '%s' (try `rul list`)", argv[1]);
		return -EINVAL;
	}
	return predict_cycle(sh, c);
}

static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "RUL prediction across all embedded cycles:");
	int total_abs_err = 0;
	for (unsigned int i = 0; i < g_cycle_count; ++i) {
		int pred;
		if (tflm_rul_predict(g_cycles[i].voltage_norm, &pred) != 0) continue;
		int err = pred - (int)g_cycles[i].true_rul;
		total_abs_err += (err < 0 ? -err : err);
		predict_cycle(sh, &g_cycles[i]);
	}
	if (g_cycle_count > 0) {
		shell_print(sh, "MAE: %d cycles (over %u samples)",
			    total_abs_err / (int)g_cycle_count, g_cycle_count);
	}
	return 0;
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = 1000;
	if (argc == 2) { n = (uint32_t)strtoul(argv[1], NULL, 10); if (!n) n = 1000; }
	if (g_cycle_count == 0) return -ENODATA;

	const float *w = g_cycles[0].voltage_norm;
	int pred;
	for (int i = 0; i < 4; ++i) tflm_rul_predict(w, &pred);

	uint32_t cyc_per_us = sys_clock_hw_cycles_per_sec() / 1000000U;
	uint32_t mn = UINT32_MAX, mx = 0;
	uint64_t total = 0;
	for (uint32_t i = 0; i < n; ++i) {
		uint32_t t0 = k_cycle_get_32();
		tflm_rul_predict(w, &pred);
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_rul,
	SHELL_CMD(list, NULL, "List embedded cycles + true RUL", cmd_list),
	SHELL_CMD(info, NULL, "Model info",                       cmd_info),
	SHELL_CMD_ARG(predict, NULL, "rul predict <cycle_name>",
		      cmd_predict, 2, 0),
	SHELL_CMD(scan, NULL, "Predict every embedded cycle + MAE", cmd_scan),
	SHELL_CMD_ARG(bench, NULL, "Inference latency [iterations]",
		      cmd_bench, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(rul, &sub_rul,
		   "Battery RUL (remaining useful life) prediction demo", NULL);
