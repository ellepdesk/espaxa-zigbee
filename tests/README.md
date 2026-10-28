# Host tests

The motion state machine (`src/motion.c`) and the battery level curve
(`src/battery_level.c`) are plain C without Zephyr dependencies, so they can
be compiled and tested on a normal machine: no board, no flashing, no nRF
Connect SDK.

## How it works

```
src/motion.c, src/battery_level.c
        │  compiled with shim.c: a fake AXA, a fake clock, extern accessors
        ▼
tests/build/libespaxa_test.so
        │  loaded with ctypes
        ▼
tests/harness.py  →  test_*.py
```

The state machine talks to the AXA, the clock and the Zigbee side only
through the callbacks in `struct motion_ops`. The shim implements them:

- **Serial:** STATUS is answered with the fake AXA's status code, every other
  command from a reply table (200 by default). Each transaction advances the
  fake clock by 300 ms, like the real ones. Every command is logged.
- **Reports:** every position offered to the Zigbee side is logged. The next
  few can be rejected, as a full ZBOSS callback queue would.
- **Clock:** `Axa.run(ms)` repeats what the AXA thread does: wait until
  `motion_next_wake()`, then `motion_tick()`, for that much fake time.

## Running

```sh
cd tests
pytest
```

`conftest.py` runs `make` for you, so there is no separate build step. To
build by hand:

```sh
make          # -> build/libespaxa_test.so
make clean
```

To see the state machine's own log: `MOTION_LOG=1 pytest -s`.

## Files

| File | Purpose |
| --- | --- |
| `Makefile` | builds the shared object from `src/` and `shim.c` |
| `shim.c` | fake AXA and clock behind `struct motion_ops`, extern accessors for ctypes |
| `harness.py` | ctypes bindings, the `Axa` wrapper, copies of the enums in `motion.h` |
| `conftest.py` | builds the library; fixtures `axa` (booted closed and locked), `axa_open`, `axa_locking` |
| `test_motion.py` | the state machine: opening, closing, locking, buttons on the AXA, failures |
| `test_battery_level.py` | the NiMH voltage to percentage curve |
| `test_harness.py` | the enum copies in `harness.py` match the compiled `motion.h` |

## Adding a test

Most tests start from a fixture:

```python
def test_open_while_locking_reverses_without_stop(axa_locking):
    axa_locking.move_to(100)
    assert axa_locking.commands == ["OPEN"]
    assert axa_locking.phase == Phase.UNLOCKING
```

- `axa.commands` is the list of commands sent, e.g. `["OPEN", "CLOSE"]`.
- `axa.reports` is every position offered to Zigbee, in open percent.
- `axa.status = Code.UNLOCKED` changes what the fake AXA answers to STATUS.
- `axa.reply(Cmd.CLOSE, Code.TIMEOUT)` makes a command fail.

If the tests need a field or function the shim does not expose yet, add it to
`shim.c` and its signature to the `signatures` dict in `harness.py`.
