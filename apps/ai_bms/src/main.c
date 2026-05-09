/*
 * ai_bms — battery-cycle anomaly detection demo.
 *
 * Boots TFLM, then idles. Embedded discharge cycles from NASA PCoE B0018
 * (a cell the AE never trained on) can be scored from the shell with
 * `bms scan` or `bms cycle aged`.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "tflm_bms.h"

LOG_MODULE_REGISTER(ai_bms, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
	int rc = tflm_bms_init();
	if (rc != 0) {
		LOG_ERR("TFLM init failed: %d", rc);
		return rc;
	}
	LOG_INF("ai_bms up — try `bms list`, `bms scan`, `bms cycle aged`");

	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	while (1) {
		gpio_pin_toggle_dt(&led);
		k_msleep(1000);
	}
	return 0;
}
