/*
 * AXA motion state machine
 *
 * Tracks what the AXA is doing (phase), whether it is locked, and the
 * estimated chain position, and decides which commands to send. Pure C
 * without Zephyr dependencies: serial transactions, the clock and position
 * reports are callbacks, so the same code runs on the board and in the host
 * tests under tests/.
 *
 * Phases:
 *   IDLE       not moving
 *   UNLOCKING  OPEN accepted while locked, the chain starts after unlock_ms
 *   OPENING    chain moving out
 *   CLOSING    chain moving in
 *   LOCKING    estimate reached closed, the AXA locks within lock_ms
 *
 * All functions must be called from one thread.
 */

#ifndef MOTION_H_
#define MOTION_H_

#include <stdbool.h>
#include <stdint.h>

enum axa_cmd {
	CMD_STATUS,
	CMD_OPEN,
	CMD_CLOSE,
	CMD_STOP,
	CMD_DEVICE,
	CMD_FIRMWARE,
};

enum axa_code {
	CODE_NONE = 0,
	CODE_TIMEOUT = -1,
	CODE_OK = 200,
	CODE_UNLOCKED = 210,
	CODE_LOCKED = 211,
	/* Assumed to mean 'closed but not fully locked' */
	CODE_LOCKED_UNSAFE = 212,
	CODE_DEVICE = 260,
	CODE_FIRMWARE = 261,
	CODE_ERROR = 502,
};

enum motion_phase {
	PHASE_IDLE,
	PHASE_UNLOCKING,
	PHASE_OPENING,
	PHASE_CLOSING,
	PHASE_LOCKING,
};

enum lock_state {
	LOCK_UNKNOWN,
	LOCK_LOCKED,
	LOCK_UNLOCKED,
};

struct motion_config {
	/* OPEN accepted while locked until the chain moves */
	uint32_t unlock_ms;
	/* Chain travel from closed to fully open */
	uint32_t open_ms;
	/* Estimate at closed until the AXA has locked */
	uint32_t lock_ms;
	/* STATUS poll interval while idle, detects use of the AXA buttons */
	uint32_t status_idle_ms;
	/* STATUS poll interval while moving, and after a move to settle */
	uint32_t status_moving_ms;
	/* Position estimate update rate while moving */
	uint32_t tick_ms;
	/* Minimum time between position reports while moving */
	uint32_t report_interval_ms;
	/* Gap between the two reports of a failed command */
	uint32_t bounce_gap_ms;
};

struct motion_ops {
	/* Run one serial transaction, returns the AXA code or CODE_TIMEOUT */
	int (*send)(void *ctx, enum axa_cmd cmd);
	/* Report the position, 0 = closed, 100 = open; 0 when accepted */
	int (*report)(void *ctx, uint8_t open_percent);
	int64_t (*now)(void *ctx);
	void (*sleep)(void *ctx, uint32_t ms);
	void *ctx;
};

struct motion {
	struct motion_config cfg;
	struct motion_ops ops;

	enum motion_phase phase;
	enum lock_state lock;
	/* 0.0 closed, 1.0 fully open */
	float position;
	float target;
	/* Start of the current phase, and the position at that time */
	int64_t phase_start;
	float phase_start_position;
	/*
	 * Last STATUS seen while idle, to detect use of the AXA buttons.
	 * CODE_NONE after a command: the next idle STATUS only sets it.
	 */
	int baseline;
	int64_t next_status;
	int64_t last_report;
	int reported;
};

/* Command as sent on the serial line, e.g. "STATUS". */
const char *motion_cmd_str(enum axa_cmd cmd);

/* Assumes closed and locked; polls STATUS on the first motion_tick(). */
void motion_init(struct motion *m, const struct motion_config *cfg,
		 const struct motion_ops *ops);

void motion_move_to(struct motion *m, uint8_t open_percent);
void motion_stop(struct motion *m);

/* Advance the estimate, poll STATUS when due, send pending reports. */
void motion_tick(struct motion *m);

/* Time at which motion_tick() needs to run next. */
int64_t motion_next_wake(const struct motion *m);

#endif /* MOTION_H_ */
