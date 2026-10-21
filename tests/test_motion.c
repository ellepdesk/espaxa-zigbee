/*
 * Host tests for the AXA motion state machine (src/motion.c)
 *
 * The fake AXA answers every command from a reply table and STATUS with
 * fake.status. Each serial transaction advances the fake clock by 300 ms,
 * like the real ones (wake delay, command gap). Commands sent and positions
 * reported are recorded for the checks.
 *
 * Run with tests/run.sh, set MOTION_LOG=1 to see the state machine's log.
 */

#include "motion.h"

#include <stdio.h>
#include <string.h>

#define TRANSACTION_MS 300

static const struct motion_config cfg = {
	.unlock_ms = 10000,
	.open_ms = 50000,
	.lock_ms = 30000,
	.status_idle_ms = 600000,
	.status_moving_ms = 2000,
	.tick_ms = 500,
	.report_interval_ms = 2000,
	.bounce_gap_ms = 500,
};

static struct {
	int64_t t;
	int status;
	int reply[CMD_FIRMWARE + 1];
	char log[1024];
	int reports[256];
	int nreports;
	int reject_reports;
} fake;

static int fake_send(void *ctx, enum axa_cmd cmd)
{
	(void)ctx;
	fake.t += TRANSACTION_MS;
	if (strlen(fake.log) + 12 < sizeof(fake.log)) {
		strcat(fake.log, motion_cmd_str(cmd));
		strcat(fake.log, " ");
	}
	return cmd == CMD_STATUS ? fake.status : fake.reply[cmd];
}

static int fake_report(void *ctx, uint8_t open_percent)
{
	(void)ctx;
	if (fake.nreports < (int)(sizeof(fake.reports) / sizeof(fake.reports[0]))) {
		fake.reports[fake.nreports++] = open_percent;
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

static struct motion m;
static int failures;
static const char *current_test;

#define CHECK(cond)                                                                        \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current_test, \
			       #cond);                                                     \
			printf("     commands: %s\n", fake.log);                           \
			failures++;                                                        \
		}                                                                          \
	} while (0)

static void clear_log(void)
{
	fake.log[0] = '\0';
	fake.nreports = 0;
}

/* Run the AXA thread's loop for ms of fake time, without requests. */
static void run(int64_t ms)
{
	int64_t end = fake.t + ms;

	for (int i = 0; i < 100000; i++) {
		int64_t wake = motion_next_wake(&m);

		if (wake > end) {
			break;
		}
		if (wake > fake.t) {
			fake.t = wake;
		}
		motion_tick(&m);
	}
	if (fake.t < end) {
		fake.t = end;
	}
}

static int last_report(void)
{
	return fake.nreports > 0 ? fake.reports[fake.nreports - 1] : -1;
}

static bool reported(int percent)
{
	for (int i = 0; i < fake.nreports; i++) {
		if (fake.reports[i] == percent) {
			return true;
		}
	}
	return false;
}

static bool sent(const char *cmd)
{
	return strstr(fake.log, cmd) != NULL;
}

/* Boot closed and locked, as confirmed by the first STATUS. */
static void boot(const char *name)
{
	current_test = name;
	memset(&fake, 0, sizeof(fake));
	for (int i = 0; i <= CMD_FIRMWARE; i++) {
		fake.reply[i] = CODE_OK;
	}
	fake.status = CODE_LOCKED;
	motion_init(&m, &cfg, &ops);
	run(1000);
	clear_log();
}

/* From boot, open fully and settle; the AXA then reports unlocked. */
static void boot_open(const char *name)
{
	boot(name);
	fake.status = CODE_UNLOCKED;
	motion_move_to(&m, 100);
	run(70000);
	clear_log();
}

/* From fully open, close until the estimate reaches closed (locking). */
static void boot_locking(const char *name)
{
	boot_open(name);
	motion_move_to(&m, 0);
	run(52000);
	clear_log();
}

/* ------------------------------------------------------------------------- */

static void test_boot_reports_closed(void)
{
	current_test = __func__;
	memset(&fake, 0, sizeof(fake));
	fake.status = CODE_LOCKED;
	motion_init(&m, &cfg, &ops);
	run(1000);
	CHECK(strcmp(fake.log, "STATUS ") == 0);
	CHECK(last_report() == 0);
	CHECK(m.lock == LOCK_LOCKED);
}

static void test_boot_unlocked_reads_as_open(void)
{
	current_test = __func__;
	memset(&fake, 0, sizeof(fake));
	fake.status = CODE_UNLOCKED;
	motion_init(&m, &cfg, &ops);
	run(1000);
	CHECK(last_report() == 100);
}

static void test_open_fully_from_locked(void)
{
	boot(__func__);
	fake.status = CODE_UNLOCKED;
	motion_move_to(&m, 100);
	CHECK(m.phase == PHASE_UNLOCKING);
	run(5000);
	CHECK(m.phase == PHASE_UNLOCKING);
	CHECK(m.position == 0.0f);
	run(65000);
	CHECK(m.phase == PHASE_IDLE);
	CHECK(last_report() == 100);
	/* The AXA stops by itself at the end */
	CHECK(!sent("STOP"));
}

static void test_open_to_half_stops(void)
{
	boot(__func__);
	fake.status = CODE_UNLOCKED;
	motion_move_to(&m, 50);
	/* OPEN acked at 0.3 s, unlocked at 10.3 s, 50% at 35.3 s */
	run(34000);
	CHECK(m.phase == PHASE_OPENING);
	CHECK(!sent("STOP"));
	run(3000);
	CHECK(m.phase == PHASE_IDLE);
	CHECK(sent("STOP"));
	CHECK(last_report() == 50);
}

static void test_stop_while_unlocking_is_not_manual_open(void)
{
	boot(__func__);
	motion_move_to(&m, 100);
	run(3000);
	/* The AXA has unlocked part way */
	fake.status = CODE_UNLOCKED;
	motion_stop(&m);
	CHECK(sent("STOP"));
	CHECK(m.phase == PHASE_IDLE);
	CHECK(m.lock == LOCK_UNKNOWN);
	run(2 * 600000);
	CHECK(m.position == 0.0f);
	CHECK(!reported(100));
	CHECK(m.lock == LOCK_UNLOCKED);
}

static void test_open_after_unknown_lock_asks_status(void)
{
	boot(__func__);
	motion_move_to(&m, 100);
	run(3000);
	fake.status = CODE_UNLOCKED;
	motion_stop(&m);
	clear_log();
	/* Before the settle STATUS: the lock state is unknown */
	motion_move_to(&m, 100);
	CHECK(strcmp(fake.log, "STATUS OPEN ") == 0);
	/* Unlocked, so the chain moves straight away */
	CHECK(m.phase == PHASE_OPENING);
}

static void test_manual_open_detected(void)
{
	boot(__func__);
	fake.status = CODE_UNLOCKED;
	run(600000 + 1000);
	CHECK(m.position == 1.0f);
	CHECK(last_report() == 100);
}

static void test_manual_close_detected(void)
{
	boot(__func__);
	fake.status = CODE_UNLOCKED;
	motion_move_to(&m, 60);
	run(60000);
	CHECK(last_report() == 60);
	fake.status = CODE_LOCKED;
	run(600000 + 1000);
	CHECK(m.position == 0.0f);
	CHECK(last_report() == 0);
}

static void test_close_reaches_locking_and_confirms(void)
{
	boot_open(__func__);
	motion_move_to(&m, 0);
	CHECK(strcmp(fake.log, "OPEN CLOSE ") == 0);
	CHECK(m.phase == PHASE_CLOSING);
	run(52000);
	CHECK(m.phase == PHASE_LOCKING);
	CHECK(last_report() == 0);
	fake.status = CODE_LOCKED;
	run(3000);
	CHECK(m.phase == PHASE_IDLE);
	CHECK(m.lock == LOCK_LOCKED);
}

static void test_repeated_close_while_locking_sends_nothing(void)
{
	boot_locking(__func__);
	CHECK(m.phase == PHASE_LOCKING);
	motion_move_to(&m, 0);
	CHECK(fake.log[0] == '\0');
	CHECK(m.phase == PHASE_LOCKING);
}

static void test_stop_while_locking_is_ignored(void)
{
	boot_locking(__func__);
	motion_stop(&m);
	CHECK(!sent("STOP"));
	CHECK(m.phase == PHASE_LOCKING);
}

static void test_open_while_locking_reverses_without_stop(void)
{
	boot_locking(__func__);
	motion_move_to(&m, 100);
	CHECK(strcmp(fake.log, "OPEN ") == 0);
	CHECK(m.phase == PHASE_UNLOCKING);
	CHECK(m.lock == LOCK_UNKNOWN);
}

static void test_lock_timer_without_confirmation(void)
{
	boot_locking(__func__);
	/* The AXA never reports locked */
	run(32000);
	CHECK(m.phase == PHASE_IDLE);
	run(2 * 600000);
	/* An unlocked STATUS right after closing is not a manual open */
	CHECK(m.position == 0.0f);
	CHECK(!reported(100));
}

static void test_close_failure_after_open_sends_stop(void)
{
	boot(__func__);
	fake.status = CODE_UNLOCKED;
	motion_move_to(&m, 60);
	run(60000);
	clear_log();
	fake.reply[CMD_CLOSE] = CODE_TIMEOUT;
	motion_move_to(&m, 0);
	CHECK(strcmp(fake.log, "OPEN CLOSE STOP ") == 0);
	CHECK(m.phase == PHASE_IDLE);
	/* Bounce: one step towards the target, then the real position */
	CHECK(fake.nreports == 2 && fake.reports[0] == 59 && fake.reports[1] == 60);
}

static void test_open_failure_bounces(void)
{
	boot(__func__);
	fake.reply[CMD_OPEN] = CODE_TIMEOUT;
	motion_move_to(&m, 100);
	CHECK(m.phase == PHASE_IDLE);
	CHECK(fake.nreports == 2 && fake.reports[0] == 1 && fake.reports[1] == 0);
}

static void test_retarget_same_direction_sends_nothing(void)
{
	boot(__func__);
	fake.status = CODE_UNLOCKED;
	motion_move_to(&m, 80);
	run(20000);
	clear_log();
	motion_move_to(&m, 90);
	CHECK(fake.log[0] == '\0');
	run(40000);
	CHECK(m.phase == PHASE_IDLE);
	CHECK(last_report() == 90);
}

static void test_reverse_direction_stops_first(void)
{
	boot(__func__);
	fake.status = CODE_UNLOCKED;
	motion_move_to(&m, 80);
	/* About 30% open */
	run(25000);
	clear_log();
	motion_move_to(&m, 10);
	CHECK(strcmp(fake.log, "STOP OPEN CLOSE ") == 0);
	CHECK(m.phase == PHASE_CLOSING);
}

static void test_rejected_report_is_retried(void)
{
	current_test = __func__;
	memset(&fake, 0, sizeof(fake));
	fake.status = CODE_LOCKED;
	fake.reject_reports = 1;
	motion_init(&m, &cfg, &ops);
	run(100);
	CHECK(m.reported == -1);
	run(1000);
	CHECK(m.reported == 0);
	CHECK(fake.nreports == 2);
}

static void test_small_move_adopts_target(void)
{
	boot(__func__);
	fake.status = CODE_UNLOCKED;
	motion_move_to(&m, 50);
	run(40000);
	clear_log();
	motion_move_to(&m, 50);
	CHECK(fake.log[0] == '\0');
	CHECK(m.phase == PHASE_IDLE);
}

int main(void)
{
	test_boot_reports_closed();
	test_boot_unlocked_reads_as_open();
	test_open_fully_from_locked();
	test_open_to_half_stops();
	test_stop_while_unlocking_is_not_manual_open();
	test_open_after_unknown_lock_asks_status();
	test_manual_open_detected();
	test_manual_close_detected();
	test_close_reaches_locking_and_confirms();
	test_repeated_close_while_locking_sends_nothing();
	test_stop_while_locking_is_ignored();
	test_open_while_locking_reverses_without_stop();
	test_lock_timer_without_confirmation();
	test_close_failure_after_open_sends_stop();
	test_open_failure_bounces();
	test_retarget_same_direction_sends_nothing();
	test_reverse_direction_stops_first();
	test_rejected_report_is_retried();
	test_small_move_adopts_target();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("All motion tests passed\n");
	return 0;
}
