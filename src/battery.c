/*
 * Battery voltage measurement, see battery.h
 */

#include "battery.h"

#include "axa.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, CONFIG_ESPAXA_LOG_LEVEL);

/* The first reading, once the AXA's boot exchange is over */
#define FIRST_MEASUREMENT_MS 20000
/* The pack recovers from the motor's load within about a minute */
#define QUIET_BEFORE_MEASURING_MS 60000
/* Retry interval while the AXA is busy or was busy recently */
#define RETRY_MS 30000

static const struct device *const divider = DEVICE_DT_GET(DT_NODELABEL(vbatt));
static battery_cb_t battery_cb;

static void measure(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct sensor_value value;
	int err;

	if (axa_quiet_ms() < QUIET_BEFORE_MEASURING_MS) {
		k_work_schedule(dwork, K_MSEC(RETRY_MS));
		return;
	}

	err = sensor_sample_fetch(divider);
	if (err == 0) {
		err = sensor_channel_get(divider, SENSOR_CHAN_VOLTAGE, &value);
	}
	if (err == 0) {
		int pack_mv = value.val1 * 1000 + value.val2 / 1000;

		battery_cb(pack_mv);
	} else {
		LOG_WRN("Cannot read the battery voltage (err %d)", err);
	}

	k_work_schedule(dwork, K_MSEC(CONFIG_ESPAXA_BATTERY_INTERVAL_MS));
}

static K_WORK_DELAYABLE_DEFINE(measure_work, measure);

int battery_init(battery_cb_t cb)
{
	if (!device_is_ready(divider)) {
		LOG_ERR("Battery voltage divider not ready");
		return -ENODEV;
	}
	battery_cb = cb;
	k_work_schedule(&measure_work, K_MSEC(FIRST_MEASUREMENT_MS));
	return 0;
}
