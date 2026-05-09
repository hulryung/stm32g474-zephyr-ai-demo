/*
 * ai_anomaly — autoencoder-based anomaly detection demo on Cortex-M4.
 *
 * - Initializes TFLM at boot.
 * - LD2 1 Hz heartbeat.
 * - All interaction via shell (cmd_anomaly.c). Run `anomaly score`,
 *   `anomaly inject pulse 0.5`, `anomaly stream on 500`, etc.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "tflm_anomaly.h"
#include "signal_gen.h"

LOG_MODULE_REGISTER(ai_anomaly, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
	signal_gen_init();

	int rc = tflm_anomaly_init();
	if (rc != 0) {
		LOG_ERR("TFLM init failed: %d", rc);
		return rc;
	}
	LOG_INF("ai_anomaly up — try `anomaly score`, `anomaly inject pulse 0.5`, "
		"`anomaly stream on 500`");

	if (!gpio_is_ready_dt(&led)) {
		LOG_WRN("LED not ready, skipping heartbeat");
		return 0;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	while (1) {
		gpio_pin_toggle_dt(&led);
		k_msleep(1000);
	}
	return 0;
}
