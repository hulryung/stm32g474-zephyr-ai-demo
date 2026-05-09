/*
 * Shell command tree: `bms ...`
 *
 *   bms list                  — list embedded sample cycles
 *   bms info                  — model + threshold + arena info
 *   bms cycle <name>          — score one of the embedded cycles (early/mid/aged)
 *   bms scan                  — run all embedded cycles, table of scores
 *   bms bench [iterations]    — inference latency
 *   bms threshold [value]     — show / set advisory warning threshold
 *
 * All cycles use int8-quantized voltage curves trained on NASA PCoE
 * B0005/B0006/B0007 healthy data; the embedded cycles are from B0018,
 * which the AE never saw during training.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>

#include "tflm_bms.h"
#include "cycles_db.h"

static float s_threshold = 0.005f;   /* tune for the trained model */

static const struct bms_cycle *find_cycle(const char *name)
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
		    "name", "cell", "idx", "capacity");
	for (unsigned int i = 0; i < g_cycle_count; ++i) {
		const struct bms_cycle *c = &g_cycles[i];
		shell_print(sh, "%-7s %-7s %-5u %.4f Ah",
			    c->name, c->cell, c->index, (double)c->capacity_ah);
	}
	return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "Model      : autoencoder (int8) trained on NASA PCoE");
	shell_print(sh, "Trained on : B0005, B0006, B0007 healthy discharge cycles");
	shell_print(sh, "Holdout    : B0018 — embedded cycles all from this cell");
	shell_print(sh, "Cycles     : %u embedded", g_cycle_count);
	shell_print(sh, "Arena      : %u bytes", tflm_bms_arena_size());
	shell_print(sh, "Threshold  : %.5f", (double)s_threshold);
	return 0;
}

static int score_cycle(const struct shell *sh, const struct bms_cycle *c)
{
	float score;
	int rc = tflm_bms_score(c->voltage, &score);
	if (rc != 0) {
		shell_error(sh, "inference failed: %d", rc);
		return rc;
	}
	const char *flag = (score > s_threshold) ? " ★ ANOMALY" : "";
	shell_print(sh, "%-7s cap=%.3f Ah  score=%.5f%s",
		    c->name, (double)c->capacity_ah, (double)score, flag);
	return 0;
}

static int cmd_cycle(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "usage: bms cycle <name>");
		shell_error(sh, "       valid names from `bms list`");
		return -EINVAL;
	}
	const struct bms_cycle *c = find_cycle(argv[1]);
	if (!c) {
		shell_error(sh, "unknown cycle '%s' (try `bms list`)", argv[1]);
		return -EINVAL;
	}
	return score_cycle(sh, c);
}

static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "Scanning all embedded cycles (threshold=%.5f):",
		    (double)s_threshold);
	for (unsigned int i = 0; i < g_cycle_count; ++i) {
		score_cycle(sh, &g_cycles[i]);
	}
	return 0;
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = 1000;
	if (argc == 2) {
		n = (uint32_t)strtoul(argv[1], NULL, 10);
		if (n == 0) { n = 1000; }
	}
	if (g_cycle_count == 0) {
		shell_error(sh, "no cycles embedded");
		return -ENODATA;
	}
	const float *window = g_cycles[0].voltage;
	float score;

	for (int i = 0; i < 4; ++i) {
		tflm_bms_score(window, &score);
	}

	uint32_t cyc_per_us = sys_clock_hw_cycles_per_sec() / 1000000U;
	uint32_t min_cyc = UINT32_MAX, max_cyc = 0;
	uint64_t total = 0;

	for (uint32_t i = 0; i < n; ++i) {
		uint32_t t0 = k_cycle_get_32();
		tflm_bms_score(window, &score);
		uint32_t dt = k_cycle_get_32() - t0;
		total += dt;
		if (dt < min_cyc) { min_cyc = dt; }
		if (dt > max_cyc) { max_cyc = dt; }
	}
	uint32_t avg = (uint32_t)(total / n);
	shell_print(sh, "iterations : %u", n);
	shell_print(sh, "avg        : %u cycles  (%u.%03u us)",
		    avg, avg / cyc_per_us, (avg * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "min        : %u cycles  (%u.%03u us)",
		    min_cyc, min_cyc / cyc_per_us, (min_cyc * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "max        : %u cycles  (%u.%03u us)",
		    max_cyc, max_cyc / cyc_per_us, (max_cyc * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "throughput : ~%u inf/sec",
		    (uint32_t)(sys_clock_hw_cycles_per_sec() / avg));
	return 0;
}

static int cmd_threshold(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_print(sh, "threshold: %.5f", (double)s_threshold);
		return 0;
	}
	float v = strtof(argv[1], NULL);
	if (v <= 0.0f) {
		shell_error(sh, "threshold must be > 0");
		return -EINVAL;
	}
	s_threshold = v;
	shell_print(sh, "threshold = %.5f", (double)s_threshold);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bms,
	SHELL_CMD(list,       NULL, "List embedded sample cycles",      cmd_list),
	SHELL_CMD(info,       NULL, "Model + arena + threshold info",   cmd_info),
	SHELL_CMD_ARG(cycle,  NULL, "Score one cycle: bms cycle <name>",
		      cmd_cycle, 2, 0),
	SHELL_CMD(scan,       NULL, "Score every embedded cycle",       cmd_scan),
	SHELL_CMD_ARG(bench,  NULL, "Inference latency [iterations]",
		      cmd_bench, 1, 1),
	SHELL_CMD_ARG(threshold, NULL,
		      "Show or set anomaly threshold",
		      cmd_threshold, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bms, &sub_bms,
		   "Battery cycle anomaly detection (NASA PCoE)", NULL);
