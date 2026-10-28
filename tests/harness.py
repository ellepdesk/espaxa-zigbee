"""ctypes bindings for the host build of the firmware's pure C modules.

``tests/Makefile`` compiles ``src/motion.c`` and ``src/battery_level.c``,
together with ``shim.c`` (a fake AXA and a fake clock), into
``build/libespaxa_test.so``. This module wraps that shared object in
something pleasant to assert against.

The fake AXA answers STATUS with ``Axa.status`` and every other command from
a reply table (200 by default). Each serial transaction advances the fake
clock by 300 ms, like the real ones. Commands sent and positions reported
are logged.

Set ``MOTION_LOG=1`` (and run pytest with ``-s``) to see the state machine's
own log.
"""

from __future__ import annotations

import ctypes
import subprocess
from enum import IntEnum
from pathlib import Path

TESTS_DIR = Path(__file__).parent
LIB_PATH = TESTS_DIR / "build" / "libespaxa_test.so"

# Timing used by every test, in ms. The firmware's defaults (Kconfig).
CONFIG = {
    "unlock_ms": 10_000,
    "open_ms": 50_000,
    "lock_ms": 30_000,
    "status_idle_ms": 600_000,
    "status_moving_ms": 2_000,
    "tick_ms": 500,
    "report_interval_ms": 2_000,
    "bounce_gap_ms": 500,
}

# Fake time per serial transaction, as in shim.c.
TRANSACTION_MS = 300


# Copies of the enums in src/motion.h; test_harness.py checks they agree.
class Phase(IntEnum):
    IDLE = 0
    UNLOCKING = 1
    OPENING = 2
    CLOSING = 3
    LOCKING = 4


class Lock(IntEnum):
    UNKNOWN = 0
    LOCKED = 1
    UNLOCKED = 2


class Cmd(IntEnum):
    STATUS = 0
    OPEN = 1
    CLOSE = 2
    STOP = 3
    DEVICE = 4
    FIRMWARE = 5


class Code(IntEnum):
    NONE = 0
    TIMEOUT = -1
    OK = 200
    UNLOCKED = 210
    LOCKED = 211
    LOCKED_UNSAFE = 212


def build() -> Path:
    """(Re)build the shared object. Cheap and idempotent; make handles it."""
    subprocess.run(["make", "--silent"], cwd=TESTS_DIR, check=True)
    return LIB_PATH


class _Lib:
    """Lazily loaded singleton around the shared object."""

    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._load()
        return cls._instance

    def _load(self):
        self.dll = ctypes.CDLL(str(build()))
        i32, i64, u8, u32, f32, cp = (
            ctypes.c_int,
            ctypes.c_int64,
            ctypes.c_uint8,
            ctypes.c_uint32,
            ctypes.c_float,
            ctypes.c_char_p,
        )
        signatures = {
            "fake_reset": ([], None),
            "fake_set_status": ([i32], None),
            "fake_set_reply": ([i32, i32], None),
            "fake_reject_reports": ([i32], None),
            "fake_time": ([], i64),
            "fake_set_time": ([i64], None),
            "fake_clear_log": ([], None),
            "fake_command_count": ([], i32),
            "fake_command": ([i32], i32),
            "fake_report_count": ([], i32),
            "fake_report_value": ([i32], i32),
            "sim_init": ([u32] * 8, None),
            "sim_move_to": ([u8], None),
            "sim_stop": ([], None),
            "sim_tick": ([], None),
            "sim_next_wake": ([], i64),
            "sim_phase": ([], i32),
            "sim_lock": ([], i32),
            "sim_position": ([], f32),
            "sim_target": ([], f32),
            "sim_reported": ([], i32),
            "sim_baseline": ([], i32),
            "sim_enum_value": ([cp], i32),
            "motion_cmd_str": ([i32], cp),
            "battery_level_percent": ([i32], u8),
        }
        for name, (argtypes, restype) in signatures.items():
            func = getattr(self.dll, name)
            func.argtypes = argtypes
            func.restype = restype


def enum_value(name: str) -> int:
    """The value of a motion.h enum constant, as compiled."""
    return _Lib().dll.sim_enum_value(name.encode())


def battery_level_percent(cell_mv: int) -> int:
    """battery_level_percent() from src/battery_level.c."""
    return _Lib().dll.battery_level_percent(cell_mv)


class Axa:
    """The motion state machine with a fake AXA behind it.

    There is one state machine in the shared object; ``start()`` resets it
    together with the fake AXA and the clock.
    """

    def __init__(self):
        self._dll = _Lib().dll

    # -- setup ---------------------------------------------------------

    def start(self, status: int = Code.LOCKED, **config):
        """Fresh state machine and fake AXA at time 0, nothing run yet."""
        cfg = {**CONFIG, **config}
        self._dll.fake_reset()
        self._dll.fake_set_status(int(status))
        self._dll.sim_init(*(cfg[key] for key in CONFIG))

    @property
    def status(self) -> int:
        raise AttributeError("status is write-only")

    @status.setter
    def status(self, code: int):
        """What the fake AXA answers to STATUS from now on."""
        self._dll.fake_set_status(int(code))

    def reply(self, cmd: Cmd, code: int):
        """What the fake AXA answers to cmd from now on."""
        self._dll.fake_set_reply(int(cmd), int(code))

    def reject_reports(self, count: int):
        """Make the next count position reports fail, as a full ZBOSS queue would."""
        self._dll.fake_reject_reports(count)

    # -- requests and time ---------------------------------------------

    def move_to(self, open_percent: int):
        self._dll.sim_move_to(open_percent)

    def stop(self):
        self._dll.sim_stop()

    def tick(self):
        self._dll.sim_tick()

    def run(self, ms: int):
        """Run the AXA thread's loop for ms of fake time, without requests."""
        end = self.time + ms
        for _ in range(100_000):
            wake = self._dll.sim_next_wake()
            if wake > end:
                break
            if wake > self.time:
                self._dll.fake_set_time(wake)
            self._dll.sim_tick()
        else:
            raise RuntimeError("state machine keeps waking without time passing")
        if self.time < end:
            self._dll.fake_set_time(end)

    @property
    def time(self) -> int:
        return self._dll.fake_time()

    # -- state ---------------------------------------------------------

    @property
    def phase(self) -> Phase:
        return Phase(self._dll.sim_phase())

    @property
    def lock(self) -> Lock:
        return Lock(self._dll.sim_lock())

    @property
    def position(self) -> float:
        return self._dll.sim_position()

    @property
    def target(self) -> float:
        return self._dll.sim_target()

    @property
    def reported(self) -> int:
        """The last report the Zigbee side accepted, -1 if none."""
        return self._dll.sim_reported()

    @property
    def baseline(self) -> int:
        return self._dll.sim_baseline()

    # -- logs ----------------------------------------------------------

    def clear_log(self):
        self._dll.fake_clear_log()

    @property
    def commands(self) -> list[str]:
        """Commands sent to the AXA, in order, e.g. ["OPEN", "CLOSE"]."""
        dll = self._dll
        return [
            dll.motion_cmd_str(dll.fake_command(i)).decode()
            for i in range(dll.fake_command_count())
        ]

    @property
    def reports(self) -> list[int]:
        """Positions offered to the Zigbee side, accepted or not, in order."""
        dll = self._dll
        return [dll.fake_report_value(i) for i in range(dll.fake_report_count())]

    @property
    def last_report(self) -> int | None:
        reports = self.reports
        return reports[-1] if reports else None
