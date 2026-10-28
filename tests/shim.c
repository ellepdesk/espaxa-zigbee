/*
 * Host test shim for the motion state machine. Nothing here is part of the
 * firmware build.
 *
 * Owns one struct motion and a fake AXA behind its callbacks:
 *   - send():   answers STATUS with fake.status and every other command from
 *               fake.reply[], and advances the fake clock by one serial
 *               transaction (300 ms, like the real ones with wake delay and
 *               command gap). Every command is logged.
 *   - report(): logs the position; rejects the next fake.reject_reports.
 *   - now() / sleep(): the fake clock.
 *
 * harness.py loads this with ctypes. battery_level_percent() is called
 * directly from battery_level.c.
 */

#include "motion.h"

#include <string.h>

#define TRANSACTION_MS 300
#define LOG_MAX 512
#define REPLY_COUNT (CMD_FIRMWARE + 1)

static struct {
	int64_t t;
	int status;
	int reply[REPLY_COUNT];
	int commands[LOG_MAX];
	int command_count;
	int reports[LOG_MAX];
	int report_count;
	int reject_reports;
} fake;

static struct motion m;

static int fake_send(void *ctx, enum axa_cmd cmd)
{
	(void)ctx;
	fake.t += TRANSACTION_MS;
	if (fake.command_count < LOG_MAX) {
		fake.commands[fake.command_count++] = cmd;
	}
	return cmd == CMD_STATUS ? fake.status : fake.reply[cmd];
}

static int fake_report(void *ctx, uint8_t open_percent)
{
	(void)ctx;
	if (fake.report_count < LOG_MAX) {
		fake.reports[fake.report_count++] = open_percent;
	}
	if (fake.reject_reports > 0) {
		fake.reject_reports--;
		return -1;
	}
	return 0;
}

static int64_t fake_now(void *ctx)
{
	(void)ctx;
	return fake.t;
}

static void fake_sleep(void *ctx, uint32_t ms)
{
	(void)ctx;
	fake.t += ms;
}

static const struct motion_ops ops = {
	.send = fake_send,
	.report = fake_report,
	.now = fake_now,
	.sleep = fake_sleep,
};

/* ------------------------------------------------------------------------- */
/* Fake AXA                                                                  */
/* ------------------------------------------------------------------------- */

/* Clock at 0, every command answered with 200, STATUS with 211, empty logs. */
void fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));
	for (int i = 0; i < REPLY_COUNT; i++) {
		fake.reply[i] = CODE_OK;
	}
	fake.status = CODE_LOCKED;
}

void fake_set_status(int code)
{
	fake.status = code;
}

void fake_set_reply(int cmd, int code)
{
	if (cmd >= 0 && cmd < REPLY_COUNT) {
		fake.reply[cmd] = code;
	}
}

void fake_reject_reports(int count)
{
	fake.reject_reports = count;
}

int64_t fake_time(void)
{
	return fake.t;
}

void fake_set_time(int64_t t)
{
	fake.t = t;
}

void fake_clear_log(void)
{
	fake.command_count = 0;
	fake.report_count = 0;
}

int fake_command_count(void)
{
	return fake.command_count;
}

int fake_command(int i)
{
	return (i >= 0 && i < fake.command_count) ? fake.commands[i] : -1;
}

int fake_report_count(void)
{
	return fake.report_count;
}

int fake_report_value(int i)
{
	return (i >= 0 && i < fake.report_count) ? fake.reports[i] : -1;
}

/* ------------------------------------------------------------------------- */
/* State machine                                                             */
/* ------------------------------------------------------------------------- */

void sim_init(uint32_t unlock_ms, uint32_t open_ms, uint32_t lock_ms, uint32_t status_idle_ms,
	      uint32_t status_moving_ms, uint32_t tick_ms, uint32_t report_interval_ms,
	      uint32_t bounce_gap_ms)
{
	const struct motion_config cfg = {
		.unlock_ms = unlock_ms,
		.open_ms = open_ms,
		.lock_ms = lock_ms,
		.status_idle_ms = status_idle_ms,
		.status_moving_ms = status_moving_ms,
		.tick_ms = tick_ms,
		.report_interval_ms = report_interval_ms,
		.bounce_gap_ms = bounce_gap_ms,
	};

	motion_init(&m, &cfg, &ops);
}

void sim_move_to(uint8_t open_percent)
{
	motion_move_to(&m, open_percent);
}

void sim_stop(void)
{
	motion_stop(&m);
}

void sim_tick(void)
{
	motion_tick(&m);
}

int64_t sim_next_wake(void)
{
	return motion_next_wake(&m);
}

int sim_phase(void)
{
	return m.phase;
}

int sim_lock(void)
{
	return m.lock;
}

float sim_position(void)
{
	return m.position;
}

float sim_target(void)
{
	return m.target;
}

int sim_reported(void)
{
	return m.reported;
}

int sim_baseline(void)
{
	return m.baseline;
}

/* Enum values from motion.h, so the Python side can check its copies. */
int sim_enum_value(const char *name)
{
	static const struct {
		const char *name;
		int value;
	} values[] = {
		{"PHASE_IDLE", PHASE_IDLE},
		{"PHASE_UNLOCKING", PHASE_UNLOCKING},
		{"PHASE_OPENING", PHASE_OPENING},
		{"PHASE_CLOSING", PHASE_CLOSING},
		{"PHASE_LOCKING", PHASE_LOCKING},
		{"LOCK_UNKNOWN", LOCK_UNKNOWN},
		{"LOCK_LOCKED", LOCK_LOCKED},
		{"LOCK_UNLOCKED", LOCK_UNLOCKED},
		{"CMD_STATUS", CMD_STATUS},
		{"CMD_OPEN", CMD_OPEN},
		{"CMD_CLOSE", CMD_CLOSE},
		{"CMD_STOP", CMD_STOP},
		{"CMD_DEVICE", CMD_DEVICE},
		{"CMD_FIRMWARE", CMD_FIRMWARE},
		{"CODE_NONE", CODE_NONE},
		{"CODE_TIMEOUT", CODE_TIMEOUT},
		{"CODE_OK", CODE_OK},
		{"CODE_UNLOCKED", CODE_UNLOCKED},
		{"CODE_LOCKED", CODE_LOCKED},
		{"CODE_LOCKED_UNSAFE", CODE_LOCKED_UNSAFE},
	};

	for (unsigned int i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		if (strcmp(values[i].name, name) == 0) {
			return values[i].value;
		}
	}
	return -9999;
}
