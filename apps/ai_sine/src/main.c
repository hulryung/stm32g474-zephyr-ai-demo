/*
 * ai_sine — TensorFlow Lite Micro on Cortex-M4 demo.
 *
 * - Initializes the TFLM interpreter at boot.
 * - Heartbeat thread blinks LD2 every 1 s so user can see the device is alive.
 * - Inference is exposed via shell commands `ai sine|bench|info` (cmd_ai.c).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "tflm_sine.h"

LOG_MODULE_REGISTER(ai_sine, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
	int rc = tflm_sine_init();
	if (rc != 0) {
		LOG_ERR("TFLM init failed: %d", rc);
		return rc;
	}
	LOG_INF("TFLM ready (arena %u B). Try `ai info`, `ai sine 1.5708`, `ai bench`.",
		tflm_sine_arena_size());

	if (!gpio_is_ready_dt(&led)) {
		LOG_WRN("LED not ready; skipping heartbeat");
		return 0;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	while (1) {
		gpio_pin_toggle_dt(&led);
		k_msleep(1000);
	}
	return 0;
}
