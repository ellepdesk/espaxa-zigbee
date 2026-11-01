# ESPAXA Zigbee

Battery powered Zigbee controller for the AXA Remote 2.0 electric window opener,
for Home Assistant ZHA. Successor of [espaxa](https://github.com/ellepdesk/espaxa)
(ESPHome / Wi-Fi).

- Seeed XIAO nRF52840, Zigbee sleepy end device
- Shows up in ZHA as a cover with position
- Commands arrive within the Zigbee poll interval (default 5 s), the parent
  router buffers them while the device sleeps
- Position estimated from travel time, locking confirmed by the AXA
- A separate lock binary sensor, locked only when the AXA itself reports 211
  Strong Locked. Set "Show as" to Lock in Home Assistant
- Detects use of the buttons on the AXA itself
- Reports the battery voltage and level of the AXA's battery pack

## Hardware

See [HARDWARE.md](HARDWARE.md) for the wiring diagram and the details.

| XIAO | Connects to |
|---|---|
| 3V3 | 3.3 V LDO (input rated 7 V or more) from the + terminal of the AXA's battery bay |
| 5V | Not connected, USB only. RJ12 pin 6 is not used either |
| GND | AXA GND (RJ12 pins 2 and 5) |
| D10 (P1.15, TX) | Tied to D9. The common line goes through a BSS138 (gate on 3V3) and 33 Ω to AXA DATA (RJ12 pins 3 and 4) |
| D9 (P1.14, RX) | Tied to D10 |
| D0 (P0.02) | Battery sense: 4.7 MΩ to battery +, 2.2 MΩ and 100 nF to GND |
| D1 (P0.03) | Factory reset: connect to GND while resetting |
| D8 (P1.13) | Bus power switch (PNP + NPN): puts the battery voltage on RJ12 pin 1, which feeds the AXA's bus pull-up, during each exchange |

The design works on both known AXA board versions and with any 4 AA cells,
NiMH or alkaline.

## Setup

Uses the nRF Connect SDK v3.4.0 with the Zigbee R23 add-on v1.4.0.

```sh
# Workspace
mkdir -p ~/ncs/zigbee-v1.4.0 && cd ~/ncs/zigbee-v1.4.0
west init -m https://github.com/nrfconnect/ncs-zigbee --mr v1.4.0
west update --narrow -o=--depth=1

# Python requirements and the ARM toolchain
uv venv --python 3.12 .venv && source .venv/bin/activate
uv pip install west -r zephyr/scripts/requirements-base.txt -r nrf/scripts/requirements-build.txt
west sdk install --version 1.0.1 -t arm-zephyr-eabi
```

## Build

From the workspace, with the venv active:

```sh
# Default build, logging on the USB serial console
west build -b xiao_ble --sysbuild -d ~/git/espaxa-zigbee/build ~/git/espaxa-zigbee

# Battery build, no USB, no console, no logging
west build -b xiao_ble --sysbuild -d ~/git/espaxa-zigbee/build-battery ~/git/espaxa-zigbee \
  -- -DEXTRA_CONF_FILE=overlay-battery.conf
```

## Tests

The motion state machine and the battery level curve are plain C, and are
unit tested on the host from Python: no board, no flashing. Needs a C
compiler, `make` and pytest.

```sh
cd tests
pytest
```

See [tests/README.md](tests/README.md). `TESTING.md` is the test plan for the
hardware with the AXA connected.

## Flash

Double-tap the reset button, the XIAO shows up as a USB drive. Copy
`build/espaxa-zigbee/zephyr/zephyr.uf2` onto it.

The console of the default build is on the USB serial port, e.g.
`picocom /dev/ttyACM0`. Logging starts 4 s after boot.

## Pairing

Enable "Add device" in ZHA and power the XIAO. It joins by itself on first boot.
To pair again, reset it with D1 connected to GND.

After a firmware update that adds a cluster, press "Reconfigure" on the
device in ZHA to pick it up.

ZHA adds an "Inverted" switch to every window covering. It sets the reversed
motor bit in the Window Covering Mode attribute, which the AXA has no use for
and this firmware ignores. Disable that entity in Home Assistant.

## Tuning

Timings are Kconfig options, set them in `prj.conf`:

| Option | Default | |
|---|---|---|
| `CONFIG_ESPAXA_UNLOCK_DURATION_MS` | 10000 | OPEN until the chain moves |
| `CONFIG_ESPAXA_OPEN_DURATION_MS` | 50000 | chain travel, closed to fully open |
| `CONFIG_ESPAXA_LOCK_DURATION_MS` | 30000 | fully closed until locked |
| `CONFIG_ESPAXA_STATUS_INTERVAL_MS` | 600000 | AXA STATUS poll while idle |
| `CONFIG_ESPAXA_MOVING_STATUS_INTERVAL_MS` | 2000 | AXA STATUS poll while moving |
| `CONFIG_ESPAXA_ZIGBEE_POLL_INTERVAL_MS` | 5000 | command delay, keep below 7.7 s |
| `CONFIG_ESPAXA_BATTERY` | y | battery voltage and level in a Power Configuration cluster |
| `CONFIG_ESPAXA_BATTERY_INTERVAL_MS` | 3600000 | battery measurement interval |
| `CONFIG_ESPAXA_BATTERY_CELLS` | 4 | NiMH cells in series |
| `CONFIG_ESPAXA_ACTIVITY_LED` | y | blue LED blinks while the AXA is busy, off in the battery build |

## Credits

- [galagaking/espaxa](https://github.com/galagaking/espaxa) by galagaking: the
  original ESPHome module for the AXA Remote 2.0. It worked out the serial
  protocol and the wiring, and found that closing needs a short OPEN first to
  release the AXA from its end position.
- [ellepdesk/espaxa](https://github.com/ellepdesk/espaxa), a fork of the above:
  a rewrite with positions estimated from travel time and the window as a
  cover. This firmware ports its logic to Zigbee.

## License

[CC0 1.0](LICENSE), the same as both projects above.

This repository contains only original code. The firmware is built with the
nRF Connect SDK, Zephyr (Apache-2.0) and the ZBOSS Zigbee stack (Nordic
5-Clause license). Those are not part of this repository, but anyone
distributing built binaries has to follow their licenses.
