/*
 * Shell command tree: `anomaly ...`
 *
 *   anomaly score                        — generate one window, run inference, print score
 *   anomaly bench [iterations]           — measure inference latency
 *   anomaly state                        — show current injection setting
 *   anomaly inject none|pulse|drift|noise [amplitude]
 *   anomaly stream on|off [period_ms]    — periodic auto-print of score
 *   anomaly threshold [value]            — show / set warning threshold (advisory)
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>

#include "tflm_anomaly.h"
#include "signal_gen.h"

static float s_threshold = 0.020f;   /* default — tune for the trained model */

/* ---- streaming thread ----------------------------------------------------*/

static struct k_thread s_stream_thr;
static K_THREAD_STACK_DEFINE(s_stream_stack, 2048);
static k_tid_t   s_stream_tid;
static bool      s_stream_on;
static uint32_t  s_stream_period_ms = 1000;
static const struct shell *s_stream_sh;

static void stream_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	float window[TFLM_ANOMALY_WINDOW];
	float score;

	while (s_stream_on) {
		signal_gen_next(window);
		if (tflm_anomaly_score(window, &score) == 0 && s_stream_sh) {
			const char *flag = (score > s_threshold) ? " ★ ANOMALY" : "";
			shell_print(s_stream_sh, "score %.5f%s",
				    (double)score, flag);
		}
		k_msleep(s_stream_period_ms);
	}
	s_stream_tid = NULL;
}

/* ---- commands ------------------------------------------------------------*/

static int cmd_score(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	float window[TFLM_ANOMALY_WINDOW];
	float score;

	signal_gen_next(window);
	int rc = tflm_anomaly_score(window, &score);
	if (rc != 0) {
		shell_error(sh, "inference failed: %d", rc);
		return rc;
	}
	const char *flag = (score > s_threshold) ? " ★ ANOMALY" : "";
	shell_print(sh, "score    : %.5f%s", (double)score, flag);
	shell_print(sh, "threshold: %.5f", (double)s_threshold);
	return 0;
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = 1000;
	if (argc == 2) {
		n = (uint32_t)strtoul(argv[1], NULL, 10);
		if (n == 0) { n = 1000; }
	}

	float window[TFLM_ANOMALY_WINDOW];
	float score;

	/* warmup */
	for (int i = 0; i < 4; ++i) {
		signal_gen_next(window);
		tflm_anomaly_score(window, &score);
	}

	uint32_t cyc_per_us = sys_clock_hw_cycles_per_sec() / 1000000U;
	uint32_t min_cyc = UINT32_MAX, max_cyc = 0;
	uint64_t total = 0;

	for (uint32_t i = 0; i < n; ++i) {
		signal_gen_next(window);
		uint32_t t0 = k_cycle_get_32();
		tflm_anomaly_score(window, &score);
		uint32_t dt = k_cycle_get_32() - t0;
		total += dt;
		if (dt < min_cyc) { min_cyc = dt; }
		if (dt > max_cyc) { max_cyc = dt; }
	}
	uint32_t avg = (uint32_t)(total / n);
	shell_print(sh, "iterations : %u", n);
	shell_print(sh, "avg        : %u cycles  (%u.%03u us)",
		    avg, avg / cyc_per_us,
		    (avg * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "min        : %u cycles  (%u.%03u us)",
		    min_cyc, min_cyc / cyc_per_us,
		    (min_cyc * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "max        : %u cycles  (%u.%03u us)",
		    max_cyc, max_cyc / cyc_per_us,
		    (max_cyc * 1000U / cyc_per_us) % 1000U);
	shell_print(sh, "throughput : ~%u inf/sec",
		    (uint32_t)(sys_clock_hw_cycles_per_sec() / avg));
	return 0;
}

static int cmd_state(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	enum injection_kind kind;
	float amp;
	signal_gen_get_injection(&kind, &amp);
	const char *names[] = {"none", "pulse", "drift", "noise"};
	shell_print(sh, "injection: %s  amplitude: %.3f",
		    names[(int)kind], (double)amp);
	shell_print(sh, "threshold: %.5f", (double)s_threshold);
	shell_print(sh, "stream   : %s (period %u ms)",
		    s_stream_on ? "on" : "off", s_stream_period_ms);
	shell_print(sh, "arena    : %u bytes", tflm_anomaly_arena_size());
	return 0;
}

static int cmd_inject(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: anomaly inject none|pulse|drift|noise [amp]");
		return -EINVAL;
	}
	float amp = (argc >= 3) ? strtof(argv[2], NULL) : 0.5f;
	if (!strcmp(argv[1], "none")) {
		signal_gen_set_injection(INJECTION_NONE, 0.0f);
	} else if (!strcmp(argv[1], "pulse")) {
		signal_gen_set_injection(INJECTION_PULSE, amp);
	} else if (!strcmp(argv[1], "drift")) {
		signal_gen_set_injection(INJECTION_DRIFT, amp);
	} else if (!strcmp(argv[1], "noise")) {
		signal_gen_set_injection(INJECTION_NOISE, amp);
	} else {
		shell_error(sh, "unknown injection '%s'", argv[1]);
		return -EINVAL;
	}
	shell_print(sh, "injection set: %s amp=%.3f", argv[1], (double)amp);
	return 0;
}

static int cmd_stream(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: anomaly stream on|off [period_ms]");
		return -EINVAL;
	}
	if (!strcmp(argv[1], "on")) {
		if (s_stream_on) {
			shell_print(sh, "already streaming");
			return 0;
		}
		if (argc >= 3) {
			s_stream_period_ms = (uint32_t)strtoul(argv[2], NULL, 10);
			if (s_stream_period_ms < 50) { s_stream_period_ms = 50; }
		}
		s_stream_sh = sh;
		s_stream_on = true;
		s_stream_tid = k_thread_create(&s_stream_thr, s_stream_stack,
			K_THREAD_STACK_SIZEOF(s_stream_stack),
			stream_entry, NULL, NULL, NULL,
			5, 0, K_NO_WAIT);
		k_thread_name_set(s_stream_tid, "anomaly_stream");
		shell_print(sh, "streaming every %u ms (Ctrl-C... or `anomaly stream off`)",
			    s_stream_period_ms);
	} else if (!strcmp(argv[1], "off")) {
		s_stream_on = false;
		shell_print(sh, "stream stopped");
	} else {
		shell_error(sh, "expected on|off");
		return -EINVAL;
	}
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_anomaly,
	SHELL_CMD(score,     NULL, "Generate one window + score it",          cmd_score),
	SHELL_CMD_ARG(bench, NULL, "Measure inference latency [n]",
		      cmd_bench, 1, 1),
	SHELL_CMD(state,     NULL, "Show current injection / threshold / stream",
		  cmd_state),
	SHELL_CMD_ARG(inject, NULL,
		      "inject none|pulse|drift|noise [amplitude]",
		      cmd_inject, 2, 1),
	SHELL_CMD_ARG(stream, NULL,
		      "stream on|off [period_ms]",
		      cmd_stream, 2, 1),
	SHELL_CMD_ARG(threshold, NULL,
		      "Show or set warning threshold",
		      cmd_threshold, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(anomaly, &sub_anomaly,
		   "Autoencoder anomaly detection demo", NULL);
