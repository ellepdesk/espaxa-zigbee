/*
 * ESPAXA Zigbee: AXA Remote 2.0 as a battery powered Zigbee window covering
 *
 * Sleepy end device with a Window Covering cluster. The AXA side lives in
 * axa.c, this file maps Zigbee commands onto it and reports the position.
 * A Binary Input cluster reports whether the AXA confirms Strong Locked.
 *
 * Zigbee lift percentage is inverted: 0 = fully open, 100 = fully closed.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <ram_pwrdn.h>

/* Enables the HA Window Covering device declarations in ZBOSS */
#define ZB_HA_DEFINE_DEVICE_WINDOW_COVERING

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zb_nrf_platform.h>
#include <zigbee/zigbee_app_utils.h>
#include <zigbee/zigbee_error_handler.h>

#include "axa.h"
#include "battery.h"
#include "battery_level.h"

LOG_MODULE_REGISTER(app, CONFIG_ESPAXA_LOG_LEVEL);

#define ESPAXA_ENDPOINT 1

/*
 * The HA Window Covering device (5 server clusters), plus Binary Input for
 * the lock and Power Configuration for the battery. Plain numbers: ZBOSS
 * pastes them into type names.
 */
#if defined(CONFIG_ESPAXA_BATTERY)
#define ESPAXA_BATTERY_CLUSTERS 1
#define ESPAXA_IN_CLUSTER_NUM 7
#else
#define ESPAXA_BATTERY_CLUSTERS 0
#define ESPAXA_IN_CLUSTER_NUM 6
#endif
#define ESPAXA_OUT_CLUSTER_NUM 0
/* Battery voltage and percentage remaining are reportable */
#define ESPAXA_REPORT_ATTR_COUNT \
	(ZB_HA_WINDOW_COVERING_REPORT_ATTR_COUNT + ZB_ZCL_BINARY_INPUT_REPORT_ATTR_COUNT + \
	 2 * ESPAXA_BATTERY_CLUSTERS)

/* ZCL: BatteryVoltage in 100 mV, 0xFF and BatteryPercentageRemaining 0xFF = unknown */
#define BATTERY_UNKNOWN 0xFF

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
	struct {
		zb_bool_t out_of_service;
		zb_bool_t present_value;
		zb_uint8_t status_flag;
	} lock;
	struct {
		zb_uint8_t voltage;
		zb_uint8_t size;
		zb_uint8_t quantity;
		zb_uint8_t rated_voltage;
		zb_uint8_t remaining;
	} power;
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

/*
 * Present value 1 = not Strong Locked, 0 = Strong Locked (211): the polarity of
 * Home Assistant's lock device class, where on means unlocked
 */
ZB_ZCL_DECLARE_BINARY_INPUT_ATTRIB_LIST(
	lock_attr_list,
	&ctx.lock.out_of_service,
	&ctx.lock.present_value,
	&ctx.lock.status_flag);

#if defined(CONFIG_ESPAXA_BATTERY)
/*
 * Declared by hand: ZBOSS's battery list macros either lack the percentage or
 * do not expand (ZB_ZCL_DECLARE_POWER_CONFIG_BATTERY_ATTRIB_LIST_EXT passes a
 * literal "bat_num"). BatteryVoltage is made reportable here, ZBOSS's own
 * descriptor for it is read-only without reporting.
 */
ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(power_config_attr_list, ZB_ZCL_POWER_CONFIG)
	{
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
		ZB_ZCL_ATTR_TYPE_U8,
		ZB_ZCL_ATTR_ACCESS_READ_ONLY | ZB_ZCL_ATTR_ACCESS_REPORTING,
		ZB_ZCL_NON_MANUFACTURER_SPECIFIC,
		(void *)&ctx.power.voltage,
	},
	ZB_SET_ATTR_DESCR_WITH_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID(&ctx.power.size, ),
	ZB_SET_ATTR_DESCR_WITH_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID(&ctx.power.quantity, ),
	ZB_SET_ATTR_DESCR_WITH_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID(
		&ctx.power.rated_voltage, ),
	ZB_SET_ATTR_DESCR_WITH_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID(
		&ctx.power.remaining, ),
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;
#endif

/* As ZB_HA_DECLARE_WINDOW_COVERING_CLUSTER_LIST, plus Power Configuration */
static zb_zcl_cluster_desc_t espaxa_clusters[] = {
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_IDENTIFY, ZB_ZCL_ARRAY_SIZE(identify_attr_list, zb_zcl_attr_t),
			    (identify_attr_list), ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_BASIC, ZB_ZCL_ARRAY_SIZE(basic_attr_list, zb_zcl_attr_t),
			    (basic_attr_list), ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_WINDOW_COVERING, ZB_ZCL_ARRAY_SIZE(window_covering_attr_list, zb_zcl_attr_t),
			    (window_covering_attr_list), ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_SCENES, ZB_ZCL_ARRAY_SIZE(scenes_attr_list, zb_zcl_attr_t),
			    (scenes_attr_list), ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_GROUPS, ZB_ZCL_ARRAY_SIZE(groups_attr_list, zb_zcl_attr_t),
			    (groups_attr_list), ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_BINARY_INPUT, ZB_ZCL_ARRAY_SIZE(lock_attr_list, zb_zcl_attr_t),
			    (lock_attr_list), ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
#if defined(CONFIG_ESPAXA_BATTERY)
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ZB_ZCL_ARRAY_SIZE(power_config_attr_list, zb_zcl_attr_t),
			    (power_config_attr_list), ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
#endif
};

/* One level of indirection, so the counts expand before ZBOSS pastes them */
#define DECLARE_SIMPLE_DESC(in_num, out_num) ZB_DECLARE_SIMPLE_DESC(in_num, out_num)
DECLARE_SIMPLE_DESC(ESPAXA_IN_CLUSTER_NUM, ESPAXA_OUT_CLUSTER_NUM);

static ZB_AF_SIMPLE_DESC_TYPE(ESPAXA_IN_CLUSTER_NUM, ESPAXA_OUT_CLUSTER_NUM) simple_desc_espaxa_ep = {
	ESPAXA_ENDPOINT,
	ZB_AF_HA_PROFILE_ID,
	ZB_HA_WINDOW_COVERING_DEVICE_ID,
	ZB_HA_DEVICE_VER_WINDOW_COVERING,
	0,
	ESPAXA_IN_CLUSTER_NUM,
	ESPAXA_OUT_CLUSTER_NUM,
	{
		ZB_ZCL_CLUSTER_ID_BASIC,
		ZB_ZCL_CLUSTER_ID_IDENTIFY,
		ZB_ZCL_CLUSTER_ID_WINDOW_COVERING,
		ZB_ZCL_CLUSTER_ID_SCENES,
		ZB_ZCL_CLUSTER_ID_GROUPS,
		ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
#if defined(CONFIG_ESPAXA_BATTERY)
		ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
#endif
	},
};

ZBOSS_DEVICE_DECLARE_REPORTING_CTX(reporting_info_espaxa_ep, ESPAXA_REPORT_ATTR_COUNT);

ZB_AF_DECLARE_ENDPOINT_DESC(espaxa_ep, ESPAXA_ENDPOINT, ZB_AF_HA_PROFILE_ID, 0, NULL,
			    ZB_ZCL_ARRAY_SIZE(espaxa_clusters, zb_zcl_cluster_desc_t),
			    espaxa_clusters, (zb_af_simple_desc_1_1_t *)&simple_desc_espaxa_ep,
			    ESPAXA_REPORT_ATTR_COUNT, reporting_info_espaxa_ep, 0, NULL);

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

	/* Not locked until a STATUS confirms it */
	ctx.lock.out_of_service = ZB_ZCL_BINARY_INPUT_OUT_OF_SERVICE_DEFAULT_VALUE;
	ctx.lock.present_value = ZB_TRUE;
	ctx.lock.status_flag = ZB_ZCL_BINARY_INPUT_STATUS_FLAG_DEFAULT_VALUE;

#if defined(CONFIG_ESPAXA_BATTERY)
	ctx.power.voltage = BATTERY_UNKNOWN;
	ctx.power.size = ZB_ZCL_POWER_CONFIG_BATTERY_SIZE_AA;
	ctx.power.quantity = CONFIG_ESPAXA_BATTERY_CELLS;
	/* Per cell, in 100 mV: NiMH 1.2 V */
	ctx.power.rated_voltage = 12;
	ctx.power.remaining = BATTERY_UNKNOWN;
#endif
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

/* Runs in the Zigbee thread, scheduled from the AXA thread. */
static void set_strong_locked(zb_uint8_t locked)
{
	zb_bool_t value = locked ? ZB_FALSE : ZB_TRUE;
	zb_zcl_status_t status = zb_zcl_set_attr_val(
		ESPAXA_ENDPOINT, ZB_ZCL_CLUSTER_ID_BINARY_INPUT, ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID, (zb_uint8_t *)&value, ZB_FALSE);

	if (status != ZB_ZCL_STATUS_SUCCESS) {
		LOG_WRN("Cannot set lock state (status %d)", status);
	}
}

/* Last lock state handed to Zigbee, -1 before the first STATUS */
static int reported_locked = -1;

static void on_axa_lock(bool strong_locked)
{
	if (strong_locked == reported_locked) {
		return;
	}
	LOG_INF("%s", strong_locked ? "Strong Locked" : "Not Strong Locked");
	/* On failure the next STATUS tries again */
	if (zigbee_schedule_callback(set_strong_locked, strong_locked) == RET_OK) {
		reported_locked = strong_locked;
	} else {
		LOG_WRN("Cannot schedule lock report");
	}
}

#if defined(CONFIG_ESPAXA_BATTERY)
/* Latest pack voltage, handed from the system work queue to the ZBOSS thread */
static atomic_t battery_mv;

static void set_attr_u8(zb_uint16_t cluster, zb_uint16_t attr, zb_uint8_t value)
{
	zb_zcl_status_t status = zb_zcl_set_attr_val(ESPAXA_ENDPOINT, cluster,
						     ZB_ZCL_CLUSTER_SERVER_ROLE, attr, &value,
						     ZB_FALSE);

	if (status != ZB_ZCL_STATUS_SUCCESS) {
		LOG_WRN("Cannot set attribute 0x%04x/0x%04x (status %d)", cluster, attr, status);
	}
}

/* Runs in the Zigbee thread, scheduled from on_battery(). */
static void set_battery_attrs(zb_uint8_t unused)
{
	int pack_mv = (int)atomic_get(&battery_mv);
	int cell_mv = pack_mv / CONFIG_ESPAXA_BATTERY_CELLS;
	zb_uint8_t voltage = BATTERY_UNKNOWN;
	zb_uint8_t remaining = BATTERY_UNKNOWN;

	ARG_UNUSED(unused);

	if (cell_mv >= BATTERY_LEVEL_MIN_VALID_CELL_MV) {
		voltage = (zb_uint8_t)MIN((pack_mv + 50) / 100, 0xFE);
		/* In half percent */
		remaining = 2 * battery_level_percent(cell_mv);
	}
	set_attr_u8(ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
		    voltage);
	set_attr_u8(ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		    ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, remaining);
}

static void on_battery(int pack_mv)
{
	int cell_mv = pack_mv / CONFIG_ESPAXA_BATTERY_CELLS;

	if (cell_mv < BATTERY_LEVEL_MIN_VALID_CELL_MV) {
		LOG_WRN("Battery %d mV, no divider on D0? Reporting unknown", pack_mv);
	} else {
		LOG_INF("Battery %d mV, %u%%", pack_mv, battery_level_percent(cell_mv));
	}
	atomic_set(&battery_mv, pack_mv);
	if (zigbee_schedule_callback(set_battery_attrs, 0) != RET_OK) {
		LOG_WRN("Cannot schedule battery report");
	}
}
#endif

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

	if (axa_init(on_axa_position, on_axa_lock) != 0) {
		LOG_ERR("AXA init failed");
	}

#if defined(CONFIG_ESPAXA_BATTERY)
	if (battery_init(on_battery) != 0) {
		LOG_ERR("Battery measurement init failed");
	}
#endif

	return 0;
}
