"""Tests for the AXA motion state machine (src/motion.c).

Timing is the firmware default: 10 s unlock, 50 s travel from closed to
fully open, 30 s lock, STATUS every 10 min idle and every 2 s while moving.
Every serial transaction takes 300 ms of fake time.
"""

from harness import Axa, Cmd, Code, Lock, Phase


# -- boot ---------------------------------------------------------------


def test_boot_polls_status_and_reports_closed():
    axa = Axa()
    axa.start(status=Code.LOCKED)
    axa.run(1000)
    assert axa.commands == ["STATUS"]
    assert axa.last_report == 0
    assert axa.lock == Lock.LOCKED


def test_boot_with_unlocked_status_reads_as_open():
    axa = Axa()
    axa.start(status=Code.UNLOCKED)
    axa.run(1000)
    assert axa.last_report == 100


# -- opening ------------------------------------------------------------


def test_open_fully_from_locked(axa):
    axa.status = Code.UNLOCKED
    axa.move_to(100)
    assert axa.phase == Phase.UNLOCKING
    axa.run(5000)
    assert axa.phase == Phase.UNLOCKING
    assert axa.position == 0.0
    axa.run(65_000)
    assert axa.phase == Phase.IDLE
    assert axa.last_report == 100
    # The AXA stops by itself at the end
    assert "STOP" not in axa.commands


def test_open_to_half_stops_there(axa):
    axa.status = Code.UNLOCKED
    axa.move_to(50)
    # OPEN acked at 0.3 s, unlocked at 10.3 s, 50% at 35.3 s
    axa.run(34_000)
    assert axa.phase == Phase.OPENING
    assert "STOP" not in axa.commands
    axa.run(3000)
    assert axa.phase == Phase.IDLE
    assert "STOP" in axa.commands
    assert axa.last_report == 50


def test_stop_while_unlocking_is_not_a_manual_open(axa):
    axa.move_to(100)
    axa.run(3000)
    # The AXA has unlocked part way
    axa.status = Code.UNLOCKED
    axa.stop()
    assert "STOP" in axa.commands
    assert axa.phase == Phase.IDLE
    assert axa.lock == Lock.UNKNOWN
    axa.run(2 * 600_000)
    assert axa.position == 0.0
    assert 100 not in axa.reports
    assert axa.lock == Lock.UNLOCKED


def test_open_with_unknown_lock_asks_status_first(axa):
    axa.move_to(100)
    axa.run(3000)
    axa.status = Code.UNLOCKED
    axa.stop()
    axa.clear_log()
    # Before the settle STATUS: the lock state is unknown
    axa.move_to(100)
    assert axa.commands == ["STATUS", "OPEN"]
    # Unlocked, so the chain moves straight away
    assert axa.phase == Phase.OPENING


def test_same_direction_only_changes_the_target(axa):
    axa.status = Code.UNLOCKED
    axa.move_to(80)
    axa.run(20_000)
    axa.clear_log()
    axa.move_to(90)
    assert axa.commands == []
    axa.run(40_000)
    assert axa.phase == Phase.IDLE
    assert axa.last_report == 90


def test_opposite_direction_stops_first(axa):
    axa.status = Code.UNLOCKED
    axa.move_to(80)
    # About 30% open
    axa.run(25_000)
    axa.clear_log()
    axa.move_to(10)
    assert axa.commands == ["STOP", "OPEN", "CLOSE"]
    assert axa.phase == Phase.CLOSING


def test_move_within_one_percent_adopts_the_target(axa):
    axa.status = Code.UNLOCKED
    axa.move_to(50)
    axa.run(40_000)
    axa.clear_log()
    axa.move_to(50)
    assert axa.commands == []
    assert axa.phase == Phase.IDLE


# -- closing and locking --------------------------------------------------


def test_close_reaches_locking_and_status_confirms(axa_open):
    axa = axa_open
    axa.move_to(0)
    assert axa.commands == ["OPEN", "CLOSE"]
    assert axa.phase == Phase.CLOSING
    axa.run(52_000)
    assert axa.phase == Phase.LOCKING
    assert axa.last_report == 0
    axa.status = Code.LOCKED
    axa.run(3000)
    assert axa.phase == Phase.IDLE
    assert axa.lock == Lock.LOCKED


def test_repeated_close_while_locking_sends_nothing(axa_locking):
    axa = axa_locking
    assert axa.phase == Phase.LOCKING
    axa.move_to(0)
    assert axa.commands == []
    assert axa.phase == Phase.LOCKING


def test_stop_while_locking_is_ignored(axa_locking):
    axa = axa_locking
    axa.stop()
    assert "STOP" not in axa.commands
    assert axa.phase == Phase.LOCKING


def test_open_while_locking_reverses_without_stop(axa_locking):
    axa = axa_locking
    axa.move_to(100)
    assert axa.commands == ["OPEN"]
    assert axa.phase == Phase.UNLOCKING
    assert axa.lock == Lock.UNKNOWN


def test_lock_timer_without_confirmation_is_not_a_manual_open(axa_locking):
    axa = axa_locking
    # The AXA never reports locked
    axa.run(32_000)
    assert axa.phase == Phase.IDLE
    axa.run(2 * 600_000)
    # An unlocked STATUS right after closing is not a manual open
    assert axa.position == 0.0
    assert 100 not in axa.reports


# -- buttons on the AXA ---------------------------------------------------


def test_manual_open_detected(axa):
    axa.status = Code.UNLOCKED
    axa.run(600_000 + 1000)
    assert axa.position == 1.0
    assert axa.last_report == 100


def test_manual_close_detected(axa):
    axa.status = Code.UNLOCKED
    axa.move_to(60)
    axa.run(60_000)
    assert axa.last_report == 60
    axa.status = Code.LOCKED
    axa.run(600_000 + 1000)
    assert axa.position == 0.0
    assert axa.last_report == 0


# -- failures -------------------------------------------------------------


def test_close_failure_after_accepted_open_sends_stop(axa):
    axa.status = Code.UNLOCKED
    axa.move_to(60)
    axa.run(60_000)
    axa.clear_log()
    axa.reply(Cmd.CLOSE, Code.TIMEOUT)
    axa.move_to(0)
    assert axa.commands == ["OPEN", "CLOSE", "STOP"]
    assert axa.phase == Phase.IDLE
    # Bounce: one step towards the target, then the real position
    assert axa.reports == [59, 60]


def test_open_failure_bounces_the_position(axa):
    axa.reply(Cmd.OPEN, Code.TIMEOUT)
    axa.move_to(100)
    assert axa.phase == Phase.IDLE
    assert axa.reports == [1, 0]


def test_rejected_report_is_retried():
    axa = Axa()
    axa.start(status=Code.LOCKED)
    axa.reject_reports(1)
    axa.run(100)
    assert axa.reported == -1
    axa.run(1000)
    assert axa.reported == 0
    assert axa.reports == [0, 0]
