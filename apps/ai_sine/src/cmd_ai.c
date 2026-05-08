/*
 * Custom shell command tree: `ai ...`
 *
 * Exposes the TFLM sine model via:
 *   ai sine  <x>           — one inference, prints predicted sin(x) and error
 *   ai bench [iterations]  — average / min / max inference time in microseconds
 *   ai info                — model status and arena size
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <math.h>

#include "tflm_sine.h"

static int cmd_ai_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Model     : sine (int8-quantized FullyConnected MLP)");
	shell_print(sh, "Framework : TensorFlow Lite Micro + CMSIS-NN");
	shell_print(sh, "Arena size: %u bytes", tflm_sine_arena_size());
	shell_print(sh, "Trained on: x in [0, 2π], target = sin(x)");
	shell_print(sh, "Use       : `ai sine 1.5708`  → expect ≈ 1.0");
	return 0;
}

static int cmd_ai_sine(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "usage: ai sine <x_radians>");
		return -EINVAL;
	}

	char *end;
	float x = strtof(argv[1], &end);
	if (*end != '\0') {
		shell_error(sh, "invalid float: '%s'", argv[1]);
		return -EINVAL;
	}

	float y;
	int rc = tflm_sine_infer(x, &y);
	if (rc != 0) {
		shell_error(sh, "inference failed: %d", rc);
		return rc;
	}

	float expected = sinf(x);
	float err = y - expected;
	shell_print(sh, "x = %.5f", (double)x);
	shell_print(sh, "predicted sin(x) = %.5f", (double)y);
	shell_print(sh, "actual    sin(x) = %.5f", (double)expected);
	shell_print(sh, "error            = %+.5f", (double)err);
	return 0;
}

static int cmd_ai_bench(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = 1000;

	if (argc == 2) {
		char *end;
		long v = strtol(argv[1], &end, 10);
		if (*end != '\0' || v < 1) {
			shell_error(sh, "iterations must be a positive integer");
			return -EINVAL;
		}
		n = (uint32_t)v;
	} else if (argc > 2) {
		shell_error(sh, "usage: ai bench [iterations]");
		return -EINVAL;
	}

	uint32_t cyc_per_us = sys_clock_hw_cycles_per_sec() / 1000000U;
	uint32_t min_cyc = UINT32_MAX, max_cyc = 0;
	uint64_t total_cyc = 0;
	float y;

	/* warm up to fault any lazy allocations / cache effects */
	for (int i = 0; i < 4; ++i) {
		tflm_sine_infer(0.0f, &y);
	}

	for (uint32_t i = 0; i < n; ++i) {
		/* sweep x across [0, 2π] so we exercise the whole input range */
		float x = (float)i * (6.2831853f / (float)n);
		uint32_t t0 = k_cycle_get_32();
		tflm_sine_infer(x, &y);
		uint32_t dt = k_cycle_get_32() - t0;
		total_cyc += dt;
		if (dt < min_cyc) { min_cyc = dt; }
		if (dt > max_cyc) { max_cyc = dt; }
	}

	uint32_t avg_cyc = (uint32_t)(total_cyc / n);
	shell_print(sh, "iterations : %u", n);
	shell_print(sh, "cpu freq   : %u Hz", sys_clock_hw_cycles_per_sec());
	shell_print(sh, "avg        : %u cycles  (%u.%03u us)",
		    avg_cyc, avg_cyc / cyc_per_us,
		    (avg_cyc * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "min        : %u cycles  (%u.%03u us)",
		    min_cyc, min_cyc / cyc_per_us,
		    (min_cyc * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "max        : %u cycles  (%u.%03u us)",
		    max_cyc, max_cyc / cyc_per_us,
		    (max_cyc * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "throughput : ~%u inf/sec",
		    (uint32_t)(sys_clock_hw_cycles_per_sec() / avg_cyc));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ai,
	SHELL_CMD(info,  NULL, "Show model + framework info",            cmd_ai_info),
	SHELL_CMD_ARG(sine,  NULL, "Predict sin(x) for given x (radians)",
		      cmd_ai_sine,  2, 0),
	SHELL_CMD_ARG(bench, NULL, "Benchmark inference time [iterations]",
		      cmd_ai_bench, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(ai, &sub_ai, "TFLM sine model commands", NULL);
