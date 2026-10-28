/*
 * AXA motion state machine, see motion.h
 *
 * Lock knowledge comes only from STATUS replies; timers only estimate the
 * position. Use of the AXA buttons is detected by comparing an idle STATUS
 * with the previous idle STATUS (the baseline). Every command clears the
 * baseline, so the first STATUS after it never counts as button use.
 */

#include "motion.h"

#include <string.h>

#ifdef __ZEPHYR__
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(axa, CONFIG_ESPAXA_LOG_LEVEL);
#else
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void host_log(const char *level, const char *fmt, ...)
{
	if (getenv("MOTION_LOG") == NULL) {
		return;
	}
	va_list ap;

	va_start(ap, fmt);
	printf("  [%s] ", level);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}
#define LOG_DBG(...) host_log("dbg", __VA_ARGS__)
#define LOG_INF(...) host_log("inf", __VA_ARGS__)
#define LOG_WRN(...) host_log("wrn", __VA_ARGS__)
#endif

static const char *const cmd_str[] = {
	[CMD_STATUS] = "STATUS",
	[CMD_OPEN] = "OPEN",
	[CMD_CLOSE] = "CLOSE",
	[CMD_STOP] = "STOP",
	[CMD_DEVICE] = "DEVICE",
	[CMD_FIRMWARE] = "FIRMWARE",
};

static const char *const phase_str[] = {
	[PHASE_IDLE] = "idle",
	[PHASE_UNLOCKING] = "unlocking",
	[PHASE_OPENING] = "opening",
	[PHASE_CLOSING] = "closing",
	[PHASE_LOCKING] = "locking",
};

/* Moves shorter than this are not worth waking the motor for */
#define MIN_MOVE 0.01f

static int64_t now(const struct motion *m)
{
	return m->ops.now(m->ops.ctx);
}

static int send(struct motion *m, enum axa_cmd cmd)
{
	return m->ops.send(m->ops.ctx, cmd);
}

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static int to_percent(float position)
{
	return (int)(position * 100.0f + 0.5f);
}

static bool is_lock_status(int code)
{
	return code == CODE_UNLOCKED || code == CODE_LOCKED || code == CODE_LOCKED_UNSAFE;
}

static bool is_locked_status(int code)
{
	return code == CODE_LOCKED || code == CODE_LOCKED_UNSAFE;
}

/* ------------------------------------------------------------------------- */
/* Reports                                                                   */
/* ------------------------------------------------------------------------- */

/* Only a report the Zigbee side accepted counts, a failed one is retried */
static void send_report(struct motion *m, int percent)
{
	m->last_report = now(m);
	if (m->ops.report(m->ops.ctx, (uint8_t)percent) == 0) {
		m->reported = percent;
	}
}

static void report(struct motion *m, bool force)
{
	int percent = to_percent(m->position);

	if (percent == m->reported) {
		return;
	}
	if (!force && now(m) - m->last_report < m->cfg.report_interval_ms) {
		return;
	}
	send_report(m, percent);
}

/*
 * A failed command leaves the position unchanged, so a normal report would be
 * deduplicated and ZHA would keep showing "opening" or "closing" until its own
 * timeout, minutes later. Report one step towards the target first and then
 * the real position again: ZHA treats the second report as a stop. The gap
 * makes sure both reports are sent.
 */
static void report_failure(struct motion *m, int direction)
{
	int percent = to_percent(m->position);
	int bounce = percent + direction;

	if (bounce >= 0 && bounce <= 100) {
		send_report(m, bounce);
		m->ops.sleep(m->ops.ctx, m->cfg.bounce_gap_ms);
	}
	send_report(m, percent);
}

/* ------------------------------------------------------------------------- */
/* Phases                                                                    */
/* ------------------------------------------------------------------------- */

static void enter(struct motion *m, enum motion_phase phase, int64_t t)
{
	if (phase != m->phase) {
		LOG_DBG("%s -> %s at %d%%", phase_str[m->phase], phase_str[phase],
			to_percent(m->position));
	}
	m->phase = phase;
	m->phase_start = t;
	m->phase_start_position = m->position;
}

/* Back to idle at a known position. With settle, poll STATUS soon. */
static void go_idle(struct motion *m, float position, bool settle)
{
	m->position = position;
	m->target = position;
	enter(m, PHASE_IDLE, now(m));
	if (settle) {
		m->next_status = now(m) + m->cfg.status_moving_ms;
	}
	report(m, true);
}

/* Update the position estimate to time t. */
static void advance(struct motion *m, int64_t t)
{
	if (m->phase == PHASE_UNLOCKING && t - m->phase_start >= m->cfg.unlock_ms) {
		/* The chain only moves once the AXA has unlocked */
		m->lock = LOCK_UNLOCKED;
		enter(m, PHASE_OPENING, m->phase_start + m->cfg.unlock_ms);
	}

	float delta = (float)(t - m->phase_start) / (float)m->cfg.open_ms;

	if (m->phase == PHASE_OPENING) {
		m->position = clamp01(m->phase_start_position + delta);
	} else if (m->phase == PHASE_CLOSING) {
		m->position = clamp01(m->phase_start_position - delta);
	}
}

/* Act on reaching the target or the end of the lock wait. */
static void check_arrival(struct motion *m, int64_t t)
{
	switch (m->phase) {
	case PHASE_OPENING:
		if (m->position >= m->target) {
			/* The AXA stops by itself when fully open */
			if (m->target < 1.0f) {
				send(m, CMD_STOP);
			}
			go_idle(m, m->target, true);
		}
		break;

	case PHASE_CLOSING:
		if (m->position > m->target) {
			break;
		}
		if (m->target > 0.0f) {
			send(m, CMD_STOP);
			go_idle(m, m->target, true);
		} else {
			/* The AXA locks by itself; the wait starts when the estimate hit 0 */
			int64_t closed_at = m->phase_start +
				(int64_t)(m->phase_start_position * (float)m->cfg.open_ms);

			m->position = 0.0f;
			enter(m, PHASE_LOCKING, closed_at < t ? closed_at : t);
			report(m, true);
		}
		break;

	case PHASE_LOCKING:
		if (t - m->phase_start >= m->cfg.lock_ms) {
			/* Not confirmed by STATUS, the settle poll will tell */
			LOG_INF("Lock not confirmed");
			m->lock = LOCK_UNKNOWN;
			go_idle(m, 0.0f, true);
		}
		break;

	default:
		break;
	}
}

/* Stop whatever the AXA is doing, keep the estimated position. */
static void halt(struct motion *m)
{
	if (m->phase == PHASE_UNLOCKING) {
		/* Stopped part way through unlocking, only STATUS can tell */
		m->lock = LOCK_UNKNOWN;
	} else if (m->position > 0.0f) {
		m->lock = LOCK_UNLOCKED;
	}
	/* Always send STOP, the AXA may be moving because of its own buttons */
	send(m, CMD_STOP);
	go_idle(m, m->position, true);
}

static void fail(struct motion *m, enum axa_cmd cmd, int code, int direction)
{
	LOG_WRN("%s failed (%d)", cmd_str[cmd], code);
	m->target = m->position;
	enter(m, PHASE_IDLE, now(m));
	report_failure(m, direction);
}

static void apply_lock_status(struct motion *m, int code)
{
	if (is_lock_status(code)) {
		m->lock = is_locked_status(code) ? LOCK_LOCKED : LOCK_UNLOCKED;
	}
}

/* Whether an OPEN from here has to wait for the AXA to unlock. */
static bool needs_unlock(struct motion *m)
{
	if (m->position > 0.0f) {
		/* The chain is out, so the AXA is unlocked */
		return false;
	}
	if (m->lock == LOCK_UNKNOWN) {
		apply_lock_status(m, send(m, CMD_STATUS));
	}
	/* Still unknown: assume locked, the usual state when closed */
	return m->lock != LOCK_UNLOCKED;
}

static void start_open(struct motion *m)
{
	bool unlock = needs_unlock(m);
	int code = send(m, CMD_OPEN);

	if (code != CODE_OK) {
		fail(m, CMD_OPEN, code, 1);
		return;
	}
	m->lock = unlock ? LOCK_UNKNOWN : LOCK_UNLOCKED;
	enter(m, unlock ? PHASE_UNLOCKING : PHASE_OPENING, now(m));
	m->next_status = now(m) + m->cfg.status_moving_ms;
}

static void start_close(struct motion *m)
{
	/* The AXA blocks at its end position, a short OPEN releases it (galagaking/espaxa) */
	int code = send(m, CMD_OPEN);

	if (code != CODE_OK) {
		fail(m, CMD_OPEN, code, -1);
		return;
	}
	code = send(m, CMD_CLOSE);
	if (code != CODE_OK) {
		/* The AXA accepted OPEN, don't leave it opening */
		send(m, CMD_STOP);
		fail(m, CMD_CLOSE, code, -1);
		return;
	}
	m->lock = LOCK_UNLOCKED;
	enter(m, PHASE_CLOSING, now(m));
	m->next_status = now(m) + m->cfg.status_moving_ms;
}

/* ------------------------------------------------------------------------- */
/* STATUS                                                                    */
/* ------------------------------------------------------------------------- */

static void handle_status(struct motion *m, int code)
{
	if (!is_lock_status(code)) {
		return;
	}

	bool locked = is_locked_status(code);

	apply_lock_status(m, code);

	switch (m->phase) {
	case PHASE_LOCKING:
	case PHASE_CLOSING:
		if (locked) {
			/* Locking confirmed, no need to wait for the estimate or timer */
			LOG_INF("Locked");
			go_idle(m, 0.0f, false);
			m->baseline = code;
		}
		break;

	case PHASE_IDLE:
		if (locked && m->position > 0.0f) {
			/* Only possible when closed, whatever the baseline */
			LOG_INF("Window was closed manually");
			go_idle(m, 0.0f, false);
		} else if (!locked && m->position == 0.0f && is_locked_status(m->baseline)) {
			/* Opened with the button on the AXA, which always opens fully */
			LOG_INF("Window was opened manually");
			go_idle(m, 1.0f, false);
		}
		m->baseline = code;
		break;

	default:
		/* Unlocking or opening: the lock state is all there is to learn */
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

const char *motion_cmd_str(enum axa_cmd cmd)
{
	return cmd_str[cmd];
}

void motion_init(struct motion *m, const struct motion_config *cfg,
		 const struct motion_ops *ops)
{
	memset(m, 0, sizeof(*m));
	m->cfg = *cfg;
	m->ops = *ops;

	/* Without a known state assume closed and locked, the first STATUS corrects it */
	m->phase = PHASE_IDLE;
	m->lock = LOCK_LOCKED;
	m->position = 0.0f;
	m->target = 0.0f;
	m->phase_start = now(m);
	/* A STATUS of 'unlocked' at boot then reads as opened, as intended */
	m->baseline = CODE_LOCKED;
	m->next_status = now(m);
	m->last_report = now(m) - m->cfg.report_interval_ms;
	m->reported = -1;
}

void motion_move_to(struct motion *m, uint8_t open_percent)
{
	float target = clamp01((float)open_percent / 100.0f);

	advance(m, now(m));
	m->baseline = CODE_NONE;

	switch (m->phase) {
	case PHASE_LOCKING: {
		if (target < MIN_MOVE) {
			/* Already closing, let it lock */
			return;
		}
		/* Reverse without STOP; mid-lock, assume a full unlock is needed */
		m->target = target;
		int code = send(m, CMD_OPEN);

		if (code != CODE_OK) {
			/* The AXA carries on locking */
			m->target = 0.0f;
			report_failure(m, 1);
			return;
		}
		m->lock = LOCK_UNKNOWN;
		enter(m, PHASE_UNLOCKING, now(m));
		m->next_status = now(m) + m->cfg.status_moving_ms;
		return;
	}

	case PHASE_UNLOCKING:
	case PHASE_OPENING:
		if (target > m->position) {
			/* Same direction, the next tick stops at the new target */
			m->target = target;
			return;
		}
		halt(m);
		break;

	case PHASE_CLOSING:
		if (target < m->position) {
			m->target = target;
			return;
		}
		halt(m);
		break;

	default:
		break;
	}

	m->target = target;

	float diff = target - m->position;

	if (diff < MIN_MOVE && diff > -MIN_MOVE) {
		/* Close enough: adopt the target, so the report matches what ZHA expects */
		go_idle(m, target, false);
		return;
	}

	if (diff > 0.0f) {
		start_open(m);
	} else {
		start_close(m);
	}
}

void motion_stop(struct motion *m)
{
	advance(m, now(m));
	m->baseline = CODE_NONE;

	if (m->phase == PHASE_LOCKING) {
		/* The window is closed already, interrupting the lock gains nothing */
		LOG_INF("Stop ignored while locking");
		report(m, true);
		return;
	}
	halt(m);
}

void motion_tick(struct motion *m)
{
	int64_t t = now(m);

	advance(m, t);
	check_arrival(m, t);

	if (now(m) >= m->next_status) {
		handle_status(m, send(m, CMD_STATUS));
		m->next_status = now(m) + (m->phase == PHASE_IDLE ? m->cfg.status_idle_ms
								  : m->cfg.status_moving_ms);
	}

	report(m, m->phase == PHASE_IDLE);
}

int64_t motion_next_wake(const struct motion *m)
{
	int64_t wake = m->next_status;
	bool moving = m->phase != PHASE_IDLE;
	bool pending = to_percent(m->position) != m->reported;

	if (moving || pending) {
		int64_t tick = now(m) + m->cfg.tick_ms;

		wake = tick < wake ? tick : wake;
	}
	return wake;
}
