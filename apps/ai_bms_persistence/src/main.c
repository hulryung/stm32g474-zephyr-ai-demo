#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "bms_state.h"

LOG_MODULE_REGISTER(ai_bms_persistence, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void) {
    int rc = bms_state_init();
    if (rc != 0) {
        LOG_ERR("bms_state_init failed: %d (continuing — NVS empty?)", rc);
    }
    const struct bms_persisted_state *s = bms_state_get();
    if (s) {
        LOG_INF("Boot #%u, restored SOC=%.2f%%, Q=%.4fAh",
                s->boot_count, (double)s->soc_pct, (double)s->Q_estimate_ah);
    } else {
        LOG_INF("First boot — no NVS state yet");
    }
    LOG_INF("Try: `persist info`, `persist demo`, `persist save 42 1.78`");

    if (gpio_is_ready_dt(&led)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
        while (1) {
            gpio_pin_toggle_dt(&led);
            k_msleep(1000);
        }
    }
    return 0;
}
