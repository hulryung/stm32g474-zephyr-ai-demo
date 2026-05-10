/*
 * ai_bms_soc — Six SOC estimators side-by-side on a Cortex-M4.
 *
 * 1. Coulomb counting     (classical baseline)
 * 2. OCV lookup           (classical, rest-state only)
 * 3. EKF + ECM            (classical production-grade)
 * 4. MLP                  (per-sample TinyML)
 * 5. LSTM                 (sequence TinyML)
 * 6. Hybrid EKF + MLP     (modern hybrid pattern)
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "soc_estimators.h"

LOG_MODULE_REGISTER(ai_bms_soc, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
    int rc;
    rc = soc_mlp_init();
    if (rc != 0) { LOG_ERR("MLP init: %d",    rc); return rc; }
    rc = soc_lstm_init();
    if (rc != 0) {
        LOG_ERR("LSTM init: %d (continuing — LSTM commands will return 0)", rc);
        /* Don't fail boot — let user run the other 5 estimators. */
    }
    rc = soc_hybrid_init();
    if (rc != 0) { LOG_ERR("Hybrid init: %d", rc); return rc; }

    LOG_INF("ai_bms_soc up — try `soc compare aged`, `soc benchall`, `soc info`");

    if (gpio_is_ready_dt(&led)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
        while (1) {
            gpio_pin_toggle_dt(&led);
            k_msleep(1000);
        }
    }
    return 0;
}
