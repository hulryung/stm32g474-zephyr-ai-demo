#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "cell_state.h"

LOG_MODULE_REGISTER(ai_bms_safety, LOG_LEVEL_INF);

extern void safety_thread_start(void);
extern void ml_thread_start(void);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void) {
    /* Initialize a healthy snapshot so the safety thread doesn't trip
     * on uninitialized zeros (which would look like 0V undervoltage). */
    struct cell_snapshot init = {
        .v = {3.85f, 3.85f, 3.85f, 3.85f},
        .i_pack = -1.0f,
        .t_max = 25.0f,
        .timestamp_ms = k_uptime_get(),
    };
    cell_state_set(&init);

    safety_thread_start();
    ml_thread_start();

    LOG_INF("ai_bms_safety up — try `safety scenario 4` then `safety status`");
    LOG_INF("Threads: safety @ priority 2 (100 Hz), ml @ priority 10 (10 Hz)");

    if (gpio_is_ready_dt(&led)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
        while (1) {
            gpio_pin_toggle_dt(&led);
            k_msleep(1000);
        }
    }
    return 0;
}
