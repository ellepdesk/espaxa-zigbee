/*
 * ESPAXA Zigbee: AXA Remote 2.0 as a battery powered Zigbee window covering
 *
 * Sleepy end device with a Window Covering cluster. The AXA side lives in
 * axa.c, this file maps Zigbee commands onto it and reports the position.
 *
 * Zigbee lift percentage is inverted: 0 = fully open, 100 = fully closed.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <ram_pwrdn.h>

/* Enables the HA Window Covering device declarations in ZBOSS */
#define ZB_HA_DEFINE_DEVICE_WINDOW_COVERING

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zb_nrf_platform.h>
#include <zigbee/zigbee_app_utils.h>
#include <zigbee/zigbee_error_handler.h>

#include "axa.h"

LOG_MODULE_REGISTER(app, CONFIG_ESPAXA_LOG_LEVEL);

#define ESPAXA_ENDPOINT 1

#define ESPAXA_MANUF_NAME "espaxa"
#define ESPAXA_MODEL_ID "AXA Remote 2.0"

static const struct gpio_dt_spec reset_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), reset_gpios);

struct espaxa_ctx {
	zb_zcl_basic_attrs_ext_t basic;
	zb_zcl_identify_attrs_t identify;
	zb_uint8_t groups_name_support;
	struct {
		zb_uint8_t scene_count;
		zb_uint8_t current_scene;
		zb_uint16_t current_group;
		zb_bool_t scene_valid;
		zb_uint8_t name_support;
	} scenes;
	struct {
		zb_uint8_t type;
		zb_uint8_t config_status;
		zb_uint8_t lift_percentage;
		zb_uint8_t tilt_percentage;
		zb_uint16_t open_limit_lift;
		zb_uint16_t closed_limit_lift;
		zb_uint16_t open_limit_tilt;
		zb_uint16_t closed_limit_tilt;
		zb_uint8_t mode;
	} wc;
};

static struct espaxa_ctx ctx;

ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST_EXT(
	basic_attr_list,
	&ctx.basic.zcl_version,
	&ctx.basic.app_version,
	&ctx.basic.stack_version,
	&ctx.basic.hw_version,
	ctx.basic.mf_name,
	ctx.basic.model_id,
	ctx.basic.date_code,
	&ctx.basic.power_source,
	ctx.basic.location_id,
	&ctx.basic.ph_env,
	ctx.basic.sw_ver);

ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(
	identify_attr_list,
	&ctx.identify.identify_time);

ZB_ZCL_DECLARE_GROUPS_ATTRIB_LIST(
	groups_attr_list,
	&ctx.groups_name_support);

ZB_ZCL_DECLARE_SCENES_ATTRIB_LIST(
	scenes_attr_list,
	&ctx.scenes.scene_count,
	&ctx.scenes.current_scene,
	&ctx.scenes.current_group,
	&ctx.scenes.scene_valid,
	&ctx.scenes.name_support);

ZB_ZCL_DECLARE_WINDOW_COVERING_CLUSTER_ATTRIB_LIST(
	window_covering_attr_list,
	&ctx.wc.type,
	&ctx.wc.config_status,
	&ctx.wc.lift_percentage,
	&ctx.wc.tilt_percentage,
	&ctx.wc.open_limit_lift,
	&ctx.wc.closed_limit_lift,
	&ctx.wc.open_limit_tilt,
	&ctx.wc.closed_limit_tilt,
	&ctx.wc.mode);

ZB_HA_DECLARE_WINDOW_COVERING_CLUSTER_LIST(
	espaxa_clusters,
	basic_attr_list,
	identify_attr_list,
	groups_attr_list,
	scenes_attr_list,
	window_covering_attr_list);

ZB_HA_DECLARE_WINDOW_COVERING_EP(
	espaxa_ep,
	ESPAXA_ENDPOINT,
	espaxa_clusters);

ZBOSS_DECLARE_DEVICE_CTX_1_EP(
	espaxa_device_ctx,
	espaxa_ep);

static void clusters_attr_init(void)
{
	ctx.basic.zcl_version = ZB_ZCL_VERSION;
	ctx.basic.power_source = ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
	ZB_ZCL_SET_STRING_VAL(ctx.basic.mf_name, ESPAXA_MANUF_NAME,
			      ZB_ZCL_STRING_CONST_SIZE(ESPAXA_MANUF_NAME));
	ZB_ZCL_SET_STRING_VAL(ctx.basic.model_id, ESPAXA_MODEL_ID,
			      ZB_ZCL_STRING_CONST_SIZE(ESPAXA_MODEL_ID));

	ctx.identify.identify_time = ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;

	ctx.wc.type = ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_ROLLERSHADE;
	ctx.wc.config_status = ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_OPERATIONAL |
			       ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_ONLINE;
	/* Closed until the first AXA STATUS says otherwise */
	ctx.wc.lift_percentage = 100;
	ctx.wc.tilt_percentage = ZB_ZCL_WINDOW_COVERING_CURRENT_POSITION_TILT_PERCENTAGE_DEFAULT_VALUE;
	ctx.wc.open_limit_lift = ZB_ZCL_WINDOW_COVERING_INSTALLED_OPEN_LIMIT_LIFT_DEFAULT_VALUE;
	ctx.wc.closed_limit_lift = ZB_ZCL_WINDOW_COVERING_INSTALLED_CLOSED_LIMIT_LIFT_DEFAULT_VALUE;
	ctx.wc.open_limit_tilt = ZB_ZCL_WINDOW_COVERING_INSTALLED_OPEN_LIMIT_TILT_DEFAULT_VALUE;
	ctx.wc.closed_limit_tilt = ZB_ZCL_WINDOW_COVERING_INSTALLED_CLOSED_LIMIT_TILT_DEFAULT_VALUE;
	ctx.wc.mode = 0;
}

/* Runs in the Zigbee thread, scheduled from the AXA thread. */
static void set_lift_percentage(zb_uint8_t lift)
{
	zb_zcl_status_t status = zb_zcl_set_attr_val(
		ESPAXA_ENDPOINT, ZB_ZCL_CLUSTER_ID_WINDOW_COVERING, ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_LIFT_PERCENTAGE_ID, &lift, ZB_FALSE);

	if (status != ZB_ZCL_STATUS_SUCCESS) {
		LOG_WRN("Cannot set lift percentage (status %d)", status);
	}
}

static int on_axa_position(uint8_t open_percent)
{
	LOG_INF("Position %u%% open", open_percent);
	if (zigbee_schedule_callback(set_lift_percentage, 100 - open_percent) != RET_OK) {
		LOG_WRN("Cannot schedule position report, retrying");
		return -EAGAIN;
	}
	return 0;
}

static void zcl_device_cb(zb_bufid_t bufid)
{
	zb_zcl_device_callback_param_t *param =
		ZB_BUF_GET_PARAM(bufid, zb_zcl_device_callback_param_t);

	param->status = RET_OK;

	switch (param->device_cb_id) {
	case ZB_ZCL_WINDOW_COVERING_UP_OPEN_CB_ID:
		LOG_INF("Open");
		axa_move_to(100);
		break;

	case ZB_ZCL_WINDOW_COVERING_DOWN_CLOSE_CB_ID:
		LOG_INF("Close");
		axa_move_to(0);
		break;

	case ZB_ZCL_WINDOW_COVERING_STOP_CB_ID:
		LOG_INF("Stop");
		axa_stop();
		break;

	case ZB_ZCL_WINDOW_COVERING_GO_TO_LIFT_PERCENTAGE_CB_ID: {
		const zb_zcl_go_to_lift_percentage_req_t *req = param->cb_param.gnr.in;

		LOG_INF("Go to lift %u%%", req->percentage_lift_value);
		axa_move_to(100 - req->percentage_lift_value);
		break;
	}

	default:
		param->status = RET_NOT_IMPLEMENTED;
		break;
	}
}

void zboss_signal_handler(zb_bufid_t bufid)
{
	zb_zdo_app_signal_hdr_t *sig_hdr = NULL;
	zb_zdo_app_signal_type_t sig = zb_get_app_signal(bufid, &sig_hdr);
	zb_ret_t status = ZB_GET_APP_SIGNAL_STATUS(bufid);

	switch (sig) {
	case ZB_BDB_SIGNAL_DEVICE_REBOOT:
	case ZB_BDB_SIGNAL_STEERING:
		ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
		if (status == RET_OK) {
			/* Worst case delay for commands from Home Assistant */
			zb_zdo_pim_set_long_poll_interval(CONFIG_ESPAXA_ZIGBEE_POLL_INTERVAL_MS);
		}
		break;

	default:
		ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
		break;
	}

	if (bufid) {
		zb_buf_free(bufid);
	}
}

/* ------------------------------------------------------------------------- */
/* Boot message                                                              */
/* ------------------------------------------------------------------------- */

static uint32_t reset_cause;

static void log_boot_banner(void)
{
	static const struct {
		uint32_t flag;
		const char *name;
	} causes[] = {
		{RESET_POR, "power on"},
		{RESET_PIN, "reset pin"},
		{RESET_SOFTWARE, "software"},
		{RESET_BROWNOUT, "brownout"},
		{RESET_WATCHDOG, "watchdog"},
		{RESET_CPU_LOCKUP, "CPU lockup"},
		{RESET_DEBUG, "debugger"},
		{RESET_LOW_POWER_WAKE, "wake from system off"},
	};

	LOG_INF("ESPAXA Zigbee, built " __DATE__ " " __TIME__);
	LOG_INF("Uptime %lld s, reset cause 0x%08x", k_uptime_get() / 1000, reset_cause);
	for (size_t i = 0; i < ARRAY_SIZE(causes); i++) {
		if (reset_cause & causes[i].flag) {
			LOG_INF("  %s", causes[i].name);
		}
	}
	LOG_INF("Zigbee poll %d ms, AXA STATUS every %d s idle / %d ms moving",
		CONFIG_ESPAXA_ZIGBEE_POLL_INTERVAL_MS, CONFIG_ESPAXA_STATUS_INTERVAL_MS / 1000,
		CONFIG_ESPAXA_MOVING_STATUS_INTERVAL_MS);
}

/*
 * The USB console drops output while no terminal is open, so whatever is
 * logged at boot is usually lost. Repeat the banner each time a terminal
 * connects (DTR goes high). Only in builds with the USB console.
 */
#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart) && \
	defined(CONFIG_UART_LINE_CTRL) && defined(CONFIG_LOG)
#define CONSOLE_WATCH_INTERVAL K_MSEC(500)

static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static void console_watch(struct k_work *work)
{
	static bool connected;
	uint32_t dtr = 0;

	/* An error (e.g. USB not configured yet) counts as not connected */
	if (uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr) != 0) {
		dtr = 0;
	}
	if (dtr && !connected) {
		log_boot_banner();
	}
	connected = dtr;
	k_work_schedule(k_work_delayable_from_work(work), CONSOLE_WATCH_INTERVAL);
}

static K_WORK_DELAYABLE_DEFINE(console_work, console_watch);

static void console_watch_start(void)
{
	k_work_schedule(&console_work, CONSOLE_WATCH_INTERVAL);
}
#else
static void console_watch_start(void)
{
}
#endif

/* Factory reset when the reset pin is held low at boot. */
static bool factory_reset_requested(void)
{
	bool requested = false;

	if (gpio_is_ready_dt(&reset_pin) && gpio_pin_configure_dt(&reset_pin, GPIO_INPUT) == 0) {
		k_msleep(10);
		requested = gpio_pin_get_dt(&reset_pin) == 1;
	}
	/* Drop the pull-up again, a jumper left in place would draw current */
	(void)gpio_pin_configure_dt(&reset_pin, GPIO_DISCONNECTED);

	return requested;
}

int main(void)
{
	if (hwinfo_get_reset_cause(&reset_cause) == 0) {
		/* The cause register accumulates until cleared */
		(void)hwinfo_clear_reset_cause();
	}
	log_boot_banner();
	console_watch_start();

	if (factory_reset_requested()) {
		LOG_INF("Factory reset");
		zigbee_erase_persistent_storage(ZB_TRUE);
	}

	zb_set_ed_timeout(ED_AGING_TIMEOUT_64MIN);
	zb_set_keepalive_timeout(ZB_MILLISECONDS_TO_BEACON_INTERVAL(30000));
	zigbee_configure_sleepy_behavior(true);

	if (IS_ENABLED(CONFIG_RAM_POWER_DOWN_LIBRARY)) {
		power_down_unused_ram();
	}

	ZB_ZCL_REGISTER_DEVICE_CB(zcl_device_cb);
	ZB_AF_REGISTER_DEVICE_CTX(&espaxa_device_ctx);
	clusters_attr_init();

	zigbee_enable();

	if (axa_init(on_axa_position) != 0) {
		LOG_ERR("AXA init failed");
	}

	return 0;
}
