/*
 * ai_bms_live — DAC1 (PA4) drives a simulated cell voltage that ADC1
 * (PA0) reads back at 1 kHz. The samples flow through a coulomb-counting
 * + EKF-style integrator. Demonstrates the production stream-processing
 * shape: ISR/timer → ring buffer → SOC worker thread.
 *
 * WIRING: jumper wire PA4 → PA0 on the Nucleo header.
 *         (PA4 is also CN7 pin 32; PA0 is CN8 pin 1 = A0.)
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>
#include <math.h>

LOG_MODULE_REGISTER(ai_bms_live, LOG_LEVEL_INF);

/* ---- DAC (PA4) ----------------------------------------------------------- */

#define DAC_NODE        DT_NODELABEL(dac1)
static const struct device *dac_dev = DEVICE_DT_GET(DAC_NODE);
#define DAC_RESOLUTION  12
#define DAC_CHANNEL_ID  1

/* ---- ADC (PA0) ----------------------------------------------------------- */

#define ADC_NODE        DT_NODELABEL(adc1)
static const struct device *adc_dev = DEVICE_DT_GET(ADC_NODE);

static const struct adc_channel_cfg adc_ch_cfg = {
    .gain             = ADC_GAIN_1,
    .reference        = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_TICKS, 47),
    .channel_id       = 1,
    .differential     = 0,
};

static int16_t adc_sample_buf;
static struct adc_sequence adc_seq = {
    .channels    = BIT(1),
    .buffer      = &adc_sample_buf,
    .buffer_size = sizeof(adc_sample_buf),
    .resolution  = 12,
};

/* ---- Ring buffer of (V, dt) ---------------------------------------------- */

struct sample {
    uint32_t t_ms;
    float    v;     /* measured voltage at ADC pin, in volts */
};
#define RB_DEPTH 256
RING_BUF_DECLARE(rb, RB_DEPTH * sizeof(struct sample));

static atomic_t g_drops;
static atomic_t g_samples_total;

/* ---- DAC scenario state -------------------------------------------------- */

static atomic_t g_running;
static atomic_t g_scenario;     /* 0 = idle, 1 = discharge sweep, 2 = step-up, 3 = sin */
static int64_t  g_t0_ms;        /* scenario start time */

#define V_REF_INTERNAL 2.5f      /* STM32G4 internal Vref+ ≈ 2.5 V */
#define DAC_FULLSCALE  4096.0f   /* 12-bit */

static void set_dac_volts(float v) {
    /* DAC output is 0..Vref+. Clamp + scale. */
    if (v < 0.0f)              v = 0.0f;
    if (v > V_REF_INTERNAL)    v = V_REF_INTERNAL;
    uint32_t code = (uint32_t)(v / V_REF_INTERNAL * DAC_FULLSCALE);
    if (code > 4095u) code = 4095u;
    dac_write_value(dac_dev, DAC_CHANNEL_ID, code);
}

static float scenario_v(int64_t t_ms) {
    /* All scenarios run in the [0..2.5 V] DAC range. We pretend this is
     * a "scaled cell voltage" — divide by 0.6 to get a 0..4.2 V cell V
     * range when displayed (or scale however you like, we don't enforce
     * a particular convention).
     */
    int s = (int)atomic_get(&g_scenario);
    int64_t dt = t_ms - g_t0_ms;
    if (dt < 0) dt = 0;
    float secs = dt / 1000.0f;

    switch (s) {
    case 1:  /* sweep down 2.4 → 0.6 over 30 s (= cell 4.0 → 1.0 V scaled) */
        return 2.4f - fminf(secs, 30.0f) / 30.0f * 1.8f;
    case 2:  /* step-up: 0.6 V for 5 s, then 2.0 V */
        return (secs < 5.0f) ? 0.6f : 2.0f;
    case 3:  /* sin around 1.5 V, ±0.4 V, 0.5 Hz */
        return 1.5f + 0.4f * sinf(secs * 2.0f * 3.14159f * 0.5f);
    default: /* idle: hold at 1.5 V */
        return 1.5f;
    }
}

/* ---- 1 kHz sampler thread ----------------------------------------------- */
/* k_timer expiry runs in ISR context, but Zephyr's adc_read is sync and
 * uses k_sem internally — not ISR-safe. So we run the sampler as a high
 * priority thread that just sleeps 1 ms between iterations. */

#define SAMPLER_STACK 2048
static K_THREAD_STACK_DEFINE(sampler_stack, SAMPLER_STACK);
static struct k_thread sampler_tid;

static void sampler_loop(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (1) {
        int64_t now = k_uptime_get();
        if (atomic_get(&g_running)) {
            set_dac_volts(scenario_v(now));
        }
        int rc = adc_read(adc_dev, &adc_seq);
        if (rc == 0) {
            int32_t mv = adc_sample_buf;
            rc = adc_raw_to_millivolts(adc_ref_internal(adc_dev),
                                       ADC_GAIN_1, 12, &mv);
            if (rc == 0) {
                struct sample s = { .t_ms = (uint32_t)now, .v = mv * 0.001f };
                uint32_t put = ring_buf_put(&rb, (uint8_t *)&s, sizeof(s));
                if (put != sizeof(s)) atomic_inc(&g_drops);
                else                  atomic_inc(&g_samples_total);
            }
        }
        k_msleep(1);  /* aim for ~1 kHz */
    }
}

/* ---- SOC worker thread (10 Hz) ------------------------------------------ */

static atomic_t g_soc_state;     /* SOC % * 100, atomic for lock-free read */
static atomic_t g_v_filt_state;  /* filtered V * 1000 (mV) */

#define WORKER_STACK 2048
static K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK);
static struct k_thread worker_tid;

static void worker_loop(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    /* Trivial integrator: SOC % decreases proportionally to (V_ref - V_filt).
     * Not a real BMS — just enough math to demonstrate that we are doing
     * *something* with the live samples (so users can visually correlate
     * scenario waveforms with SOC trace).
     */
    float v_filt = 1.5f;
    float soc = 100.0f;

    while (1) {
        /* drain whatever the timer captured this tick */
        struct sample s;
        int processed = 0;
        while (ring_buf_get(&rb, (uint8_t *)&s, sizeof(s)) == sizeof(s)) {
            v_filt = 0.95f * v_filt + 0.05f * s.v;       /* 1st-order IIR */
            processed++;
        }

        /* Toy "discharge" model: lower V → less SOC. Just for the demo. */
        if (atomic_get(&g_running) && processed > 0) {
            float v_norm = (v_filt - 0.6f) / (2.4f - 0.6f);  /* map [0.6..2.4]→[0..1] */
            if (v_norm < 0.0f) v_norm = 0.0f;
            if (v_norm > 1.0f) v_norm = 1.0f;
            /* Heavy LPF blends toward target */
            float target = v_norm * 100.0f;
            soc = 0.95f * soc + 0.05f * target;
        }

        atomic_set(&g_soc_state, (atomic_val_t)(soc * 100.0f));
        atomic_set(&g_v_filt_state, (atomic_val_t)(v_filt * 1000.0f));
        k_msleep(100);
    }
}

/* ---- Shell --------------------------------------------------------------- */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

static int cmd_simulate(const struct shell *sh, size_t a, char **v) {
    if (a < 2) {
        shell_error(sh, "usage: live simulate <stop|sweep|step|sin>");
        return -EINVAL;
    }
    if (!strcmp(v[1], "stop")) {
        atomic_set(&g_scenario, 0);
        atomic_set(&g_running, 0);
        set_dac_volts(1.5f);
        shell_print(sh, "stopped (DAC held at 1.5 V)");
    } else if (!strcmp(v[1], "sweep")) {
        atomic_set(&g_scenario, 1);
        g_t0_ms = k_uptime_get();
        atomic_set(&g_running, 1);
        shell_print(sh, "sweep: V down 2.4 → 0.6 over 30 s");
    } else if (!strcmp(v[1], "step")) {
        atomic_set(&g_scenario, 2);
        g_t0_ms = k_uptime_get();
        atomic_set(&g_running, 1);
        shell_print(sh, "step: V = 0.6 then 2.0 (+5 s)");
    } else if (!strcmp(v[1], "sin")) {
        atomic_set(&g_scenario, 3);
        g_t0_ms = k_uptime_get();
        atomic_set(&g_running, 1);
        shell_print(sh, "sin: V = 1.5 ± 0.4 @ 0.5 Hz");
    } else {
        shell_error(sh, "unknown scenario '%s'", v[1]);
        return -EINVAL;
    }
    return 0;
}

static int cmd_status(const struct shell *sh, size_t a, char **v) {
    ARG_UNUSED(a); ARG_UNUSED(v);
    long v_mv = atomic_get(&g_v_filt_state);
    long soc_x100 = atomic_get(&g_soc_state);
    long total = atomic_get(&g_samples_total);
    long drops = atomic_get(&g_drops);
    int s = (int)atomic_get(&g_scenario);
    const char *names[] = {"idle", "sweep", "step", "sin"};
    shell_print(sh, "Scenario     : %s%s",
                names[s], atomic_get(&g_running) ? " (running)" : " (stopped)");
    shell_print(sh, "DAC target   : %.3f V",
                (double)scenario_v(k_uptime_get()));
    shell_print(sh, "ADC filtered : %.3f V", (double)(v_mv / 1000.0f));
    shell_print(sh, "Toy SOC      : %.2f %%", (double)(soc_x100 / 100.0f));
    shell_print(sh, "Samples      : %ld captured / %ld dropped (rb full)", total, drops);
    return 0;
}

static int cmd_loopback(const struct shell *sh, size_t a, char **v) {
    /* Quick check the jumper wire is in. Set DAC to 4 different voltages
     * over 800 ms and report what ADC saw. Operator can eyeball the match.
     */
    ARG_UNUSED(a); ARG_UNUSED(v);
    const float test_pts[] = {0.5f, 1.0f, 1.5f, 2.0f};
    shell_print(sh, "loopback test (PA4 → PA0):");
    /* Pause the scenario timer so it doesn't fight us */
    atomic_t prev_running = g_running;
    atomic_set(&g_running, 0);
    for (int i = 0; i < (int)ARRAY_SIZE(test_pts); ++i) {
        set_dac_volts(test_pts[i]);
        k_msleep(50);   /* DAC slew + ADC settle */
        int32_t mv = 0;
        for (int k = 0; k < 16; ++k) {
            adc_read(adc_dev, &adc_seq);
            int32_t this_mv = adc_sample_buf;
            adc_raw_to_millivolts(adc_ref_internal(adc_dev),
                                  ADC_GAIN_1, 12, &this_mv);
            mv += this_mv;
        }
        mv /= 16;
        shell_print(sh, "  DAC = %.3f V  →  ADC = %.3f V  (err = %+.3f V)",
                    (double)test_pts[i], (double)(mv/1000.0f),
                    (double)((mv/1000.0f) - test_pts[i]));
    }
    atomic_set(&g_running, prev_running);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_live,
    SHELL_CMD_ARG(simulate, NULL, "live simulate <stop|sweep|step|sin>",
                  cmd_simulate, 2, 0),
    SHELL_CMD(status, NULL, "Read SOC + filtered V + sample stats", cmd_status),
    SHELL_CMD(loopback, NULL, "DAC↔ADC wiring sanity check",        cmd_loopback),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(live, &sub_live, "DAC→ADC live BMS pipeline demo", NULL);

/* ---- Setup --------------------------------------------------------------- */

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void) {
    if (!device_is_ready(adc_dev)) {
        LOG_ERR("ADC not ready"); return -1;
    }
    if (!device_is_ready(dac_dev)) {
        LOG_ERR("DAC not ready"); return -1;
    }
    int rc = adc_channel_setup(adc_dev, &adc_ch_cfg);
    if (rc != 0) { LOG_ERR("adc_channel_setup: %d", rc); return rc; }

    struct dac_channel_cfg dac_cfg = {
        .channel_id  = DAC_CHANNEL_ID,
        .resolution  = DAC_RESOLUTION,
        .buffered    = true,
    };
    rc = dac_channel_setup(dac_dev, &dac_cfg);
    if (rc != 0) { LOG_ERR("dac_channel_setup: %d", rc); return rc; }

    set_dac_volts(1.5f);  /* idle DC level */

    /* SOC worker @ priority 8 — well below shell, well above idle */
    k_thread_create(&worker_tid, worker_stack, WORKER_STACK,
                    worker_loop, NULL, NULL, NULL,
                    8, 0, K_NO_WAIT);
    k_thread_name_set(&worker_tid, "soc_worker");

    /* 1 kHz sampler thread @ priority 4 (above worker, below safety
     * priorities a real BMS would have) */
    k_thread_create(&sampler_tid, sampler_stack, SAMPLER_STACK,
                    sampler_loop, NULL, NULL, NULL,
                    4, 0, K_NO_WAIT);
    k_thread_name_set(&sampler_tid, "sampler_1khz");

    LOG_INF("ai_bms_live up — connect PA4 to PA0 with a jumper, then:");
    LOG_INF("  live loopback                — verify wiring");
    LOG_INF("  live simulate sweep          — start a discharge sweep");
    LOG_INF("  live status                  — see filtered V + toy SOC");

    if (gpio_is_ready_dt(&led)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
        while (1) {
            gpio_pin_toggle_dt(&led);
            k_msleep(1000);
        }
    }
    return 0;
}
