/*
 * AXA Remote 2.0 driver
 *
 * Serial protocol: 19200 baud, 8N2. The AXA sleeps between commands, a bare
 * newline wakes it up. Each command is answered with a line starting with a
 * three digit code, e.g. "211 Locked". The AXA only reports locked or
 * unlocked, the position in between is estimated from travel time.
 *
 * Everything here runs in the AXA thread, which blocks on the serial line and
 * on its request queue. The UART is suspended between transactions, its
 * receiver would otherwise keep the high frequency clock running. What to
 * send when, and the position estimate, live in motion.c.
 */

#include "axa.h"
#include "motion.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(axa, CONFIG_ESPAXA_LOG_LEVEL);

#define WAKE_DELAY_MS 50
#define RESPONSE_TIMEOUT_MS 1000
/* Pause between transactions, the AXA ignores commands sent back to back */
#define COMMAND_GAP_MS 200
#define MAX_RETRIES 2
/* Position estimate update rate while moving */
#define MOTION_TICK_MS 500
/* Minimum time between position reports while moving */
#define REPORT_INTERVAL_MS 2000
/* Gap between the two reports of a failed command, so both are sent */
#define FAILURE_BOUNCE_GAP_MS 500
#define LINE_MAX 64
/* Activity LED toggle period, 2 Hz blink */
#define LED_TOGGLE_MS 250

#define THREAD_STACK_SIZE 2048
#define THREAD_PRIORITY 8

struct request {
	bool stop;
	uint8_t open_percent;
};

/* Holds one request: only the latest one matters, a new one replaces it */
K_MSGQ_DEFINE(request_q, sizeof(struct request), 1, 4);
K_MSGQ_DEFINE(rx_q, sizeof(uint8_t), 128, 1);

K_THREAD_STACK_DEFINE(axa_stack, THREAD_STACK_SIZE);
static struct k_thread axa_thread_data;

static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(axa_uart));
static axa_position_cb_t position_cb;
static struct motion motion;
static bool in_transaction;

/* ------------------------------------------------------------------------- */
/* Activity                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Read by other threads through axa_quiet_ms(). Not busy until axa_init()
 * succeeds, so a missing AXA UART does not block the battery measurement.
 */
static atomic_t busy_flag;
static atomic_t quiet_since_ms;

#define ACTIVITY_LED_NODE DT_ALIAS(activity_led)

#if defined(CONFIG_ESPAXA_ACTIVITY_LED) && DT_NODE_EXISTS(ACTIVITY_LED_NODE)
static const struct gpio_dt_spec activity_led = GPIO_DT_SPEC_GET(ACTIVITY_LED_NODE, gpios);

static void led_toggle(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	(void)gpio_pin_toggle_dt(&activity_led);
}

static K_TIMER_DEFINE(led_timer, led_toggle, NULL);

static void led_init(void)
{
	if (!gpio_is_ready_dt(&activity_led) ||
	    gpio_pin_configure_dt(&activity_led, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_WRN("Activity LED not available");
	}
}

/* Blink while busy, off otherwise */
static void led_set(bool busy)
{
	if (busy) {
		(void)gpio_pin_set_dt(&activity_led, 1);
		k_timer_start(&led_timer, K_MSEC(LED_TOGGLE_MS), K_MSEC(LED_TOGGLE_MS));
	} else {
		k_timer_stop(&led_timer);
		(void)gpio_pin_set_dt(&activity_led, 0);
	}
}
#else
static void led_init(void)
{
}

static void led_set(bool busy)
{
	ARG_UNUSED(busy);
}
#endif

/* Busy from the start of the boot exchange */
static void activity_init(void)
{
	led_init();
	atomic_set(&busy_flag, 1);
	led_set(true);
}

/* Busy while talking to the AXA or while it moves. */
static void activity_update(void)
{
	bool busy = in_transaction || motion.phase != PHASE_IDLE;

	if (busy == (bool)atomic_get(&busy_flag)) {
		return;
	}
	if (!busy) {
		atomic_set(&quiet_since_ms, (atomic_val_t)k_uptime_get_32());
	}
	atomic_set(&busy_flag, busy);
	led_set(busy);
}

uint32_t axa_quiet_ms(void)
{
	if (atomic_get(&busy_flag)) {
		return 0;
	}
	return k_uptime_get_32() - (uint32_t)atomic_get(&quiet_since_ms);
}

/* ------------------------------------------------------------------------- */
/* Serial                                                                    */
/* ------------------------------------------------------------------------- */

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t buf[16];

	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}
		for (int i = 0; i < n; i++) {
			(void)k_msgq_put(&rx_q, &buf[i], K_NO_WAIT);
		}
	}
}

static void uart_power(bool on)
{
	if (on) {
		(void)pm_device_action_run(uart, PM_DEVICE_ACTION_RESUME);
		uart_irq_rx_enable(uart);
	} else {
		uart_irq_rx_disable(uart);
		(void)pm_device_action_run(uart, PM_DEVICE_ACTION_SUSPEND);
	}
}

static void write_str(const char *str)
{
	while (*str) {
		uart_poll_out(uart, *str++);
	}
}

/* Read one non-empty line, returns its length or -1 on timeout. */
static int read_line(char *line, size_t size, k_timepoint_t deadline)
{
	size_t len = 0;
	uint8_t c;

	while (k_msgq_get(&rx_q, &c, sys_timepoint_timeout(deadline)) == 0) {
		if (c == '\r' || c == '\n') {
			if (len > 0) {
				line[len] = '\0';
				return len;
			}
		} else if (len < size - 1) {
			line[len++] = c;
		}
	}
	return -1;
}

/*
 * Wake the AXA with a newline, drop whatever it replied, send the command and
 * wait for a line starting with a status code. Echo and help text is skipped.
 */
/*
 * TX and RX share one bus line, so RX hears everything we send: the wake
 * newline's echo is purged with the AXA's reply to it, the command's echo
 * comes back as a line of its own and is skipped below. No echo at all
 * means the bus line itself is broken, not that the AXA is silent.
 */
static int transact(enum axa_cmd cmd, char *line, size_t size)
{
	int code = CODE_TIMEOUT;
	bool echo_seen = false;

	in_transaction = true;
	activity_update();
	uart_power(true);

	for (int attempt = 0; attempt <= MAX_RETRIES && code == CODE_TIMEOUT; attempt++) {
		if (attempt > 0) {
			LOG_DBG("No response to %s, retry %d", motion_cmd_str(cmd), attempt);
		}
		write_str("\r\n");
		k_msleep(WAKE_DELAY_MS);
		k_msgq_purge(&rx_q);

		write_str(motion_cmd_str(cmd));
		write_str("\r\n");

		k_timepoint_t deadline = sys_timepoint_calc(K_MSEC(RESPONSE_TIMEOUT_MS));

		while (read_line(line, size, deadline) >= 0) {
			if (isdigit((unsigned char)line[0]) && isdigit((unsigned char)line[1]) &&
			    isdigit((unsigned char)line[2])) {
				code = atoi(line);
				break;
			}
			if (strcmp(line, motion_cmd_str(cmd)) == 0) {
				/* Our own transmission, possibly the AXA's echo too */
				echo_seen = true;
				continue;
			}
			LOG_DBG("Skipping '%s'", line);
		}
	}

	uart_power(false);
	in_transaction = false;
	activity_update();

	if (code == CODE_TIMEOUT && !echo_seen) {
		LOG_WRN("No echo of %s on the bus, check the D5/D6 wiring", motion_cmd_str(cmd));
	}

	if (code == CODE_TIMEOUT) {
		line[0] = '\0';
	}
	LOG_DBG("%s -> %d '%s'", motion_cmd_str(cmd), code, line);

	k_msleep(COMMAND_GAP_MS);
	return code;
}

static int send_cmd(enum axa_cmd cmd)
{
	char line[LINE_MAX];

	return transact(cmd, line, sizeof(line));
}

/* ------------------------------------------------------------------------- */
/* Thread                                                                    */
/* ------------------------------------------------------------------------- */

static int ops_send(void *ctx, enum axa_cmd cmd)
{
	ARG_UNUSED(ctx);
	return send_cmd(cmd);
}

static int ops_report(void *ctx, uint8_t open_percent)
{
	ARG_UNUSED(ctx);
	return position_cb(open_percent);
}

static int64_t ops_now(void *ctx)
{
	ARG_UNUSED(ctx);
	return k_uptime_get();
}

static void ops_sleep(void *ctx, uint32_t ms)
{
	ARG_UNUSED(ctx);
	k_msleep(ms);
}

static const struct motion_config motion_cfg = {
	.unlock_ms = CONFIG_ESPAXA_UNLOCK_DURATION_MS,
	.open_ms = CONFIG_ESPAXA_OPEN_DURATION_MS,
	.lock_ms = CONFIG_ESPAXA_LOCK_DURATION_MS,
	.status_idle_ms = CONFIG_ESPAXA_STATUS_INTERVAL_MS,
	.status_moving_ms = CONFIG_ESPAXA_MOVING_STATUS_INTERVAL_MS,
	.tick_ms = MOTION_TICK_MS,
	.report_interval_ms = REPORT_INTERVAL_MS,
	.bounce_gap_ms = FAILURE_BOUNCE_GAP_MS,
};

static const struct motion_ops motion_ops = {
	.send = ops_send,
	.report = ops_report,
	.now = ops_now,
	.sleep = ops_sleep,
};

static void axa_thread(void *p1, void *p2, void *p3)
{
	char line[LINE_MAX];
	struct request req;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (transact(CMD_DEVICE, line, sizeof(line)) == CODE_DEVICE) {
		LOG_INF("Device: %s", line);
	}
	if (transact(CMD_FIRMWARE, line, sizeof(line)) == CODE_FIRMWARE) {
		LOG_INF("Firmware: %s", line);
	}

	/* Polls STATUS and sends the first report on the first tick */
	motion_init(&motion, &motion_cfg, &motion_ops);

	while (true) {
		int64_t timeout = motion_next_wake(&motion) - k_uptime_get();

		if (k_msgq_get(&request_q, &req, K_MSEC(MAX(timeout, 0))) == 0) {
			if (req.stop) {
				motion_stop(&motion);
			} else {
				motion_move_to(&motion, req.open_percent);
			}
		}
		motion_tick(&motion);
		activity_update();
	}
}

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

static void queue_request(const struct request *req)
{
	/* Replace a request that has not been picked up yet */
	if (k_msgq_put(&request_q, req, K_NO_WAIT) != 0) {
		k_msgq_purge(&request_q);
		(void)k_msgq_put(&request_q, req, K_NO_WAIT);
	}
}

void axa_move_to(uint8_t open_percent)
{
	struct request req = {.stop = false, .open_percent = MIN(open_percent, 100)};

	queue_request(&req);
}

void axa_stop(void)
{
	struct request req = {.stop = true};

	queue_request(&req);
}

int axa_init(axa_position_cb_t cb)
{
	const struct uart_config cfg = {
		.baudrate = 19200,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_2,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	int err;

	if (!device_is_ready(uart)) {
		LOG_ERR("AXA UART not ready");
		return -ENODEV;
	}

	err = uart_configure(uart, &cfg);
	if (err) {
		LOG_ERR("Cannot configure AXA UART (err %d)", err);
		return err;
	}

	err = uart_irq_callback_set(uart, uart_isr);
	if (err) {
		LOG_ERR("Cannot set AXA UART callback (err %d)", err);
		return err;
	}
	uart_power(false);

	position_cb = cb;
	activity_init();

	k_thread_create(&axa_thread_data, axa_stack, K_THREAD_STACK_SIZEOF(axa_stack), axa_thread,
			NULL, NULL, NULL, THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&axa_thread_data, "axa");

	return 0;
}
