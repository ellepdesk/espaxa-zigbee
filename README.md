# ESPAXA Zigbee

Battery powered Zigbee controller for the AXA Remote 2.0 electric window opener,
for Home Assistant ZHA. Successor of [espaxa](https://github.com/ellepdesk/espaxa)
(ESPHome / Wi-Fi).

- Seeed XIAO nRF52840, Zigbee sleepy end device
- Shows up in ZHA as a cover with position
- Commands arrive within the Zigbee poll interval (default 5 s), the parent
  router buffers them while the device sleeps
- Position estimated from travel time, locking confirmed by the AXA
- Detects use of the buttons on the AXA itself

## Hardware

| XIAO | Connects to |
|---|---|
| D6 (P1.11, TX) | Tied to D5, the common line goes through the level shifter to AXA data |
| D5 (P0.05, RX) | Tied to D6 |
| 5V | 4x NiMH pack (shared with the AXA) |
| GND | AXA GND |
| D1 (P0.03) | Factory reset: connect to GND while resetting |

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

The motion state machine has host tests that need only a C compiler:

```sh
tests/run.sh
```

`TESTING.md` is the test plan for the hardware with the AXA connected.

## Flash

Double-tap the reset button, the XIAO shows up as a USB drive. Copy
`build/espaxa-zigbee/zephyr/zephyr.uf2` onto it.

The console of the default build is on the USB serial port, e.g.
`picocom /dev/ttyACM0`. Logging starts 4 s after boot.

## Pairing

Enable "Add device" in ZHA and power the XIAO. It joins by itself on first boot.
To pair again, reset it with D1 connected to GND.

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
