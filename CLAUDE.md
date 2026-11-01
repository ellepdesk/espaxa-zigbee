# ESPAXA Zigbee

Firmware for a battery powered Zigbee controller for the AXA Remote 2.0 window
opener, used from Home Assistant ZHA. It runs on a Seeed XIAO nRF52840 and is
built with nRF Connect SDK v3.4.0 and the Zigbee R23 add-on v1.4.0.

Status: it compiles, but has not yet been run on hardware or paired with ZHA.

## Build, flash, run

The workspace is `~/ncs/zigbee-v1.4.0`, outside this repo. Activate its venv first:

```sh
cd ~/ncs/zigbee-v1.4.0 && source .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1

# Default build: USB serial console + debug logging (current bring-up mode)
west build -b xiao_ble --sysbuild -d ~/git/espaxa-zigbee/build ~/git/espaxa-zigbee -p always

# Battery build: no USB, console or logging
west build -b xiao_ble --sysbuild -d ~/git/espaxa-zigbee/build-battery ~/git/espaxa-zigbee -p always \
  -- -DEXTRA_CONF_FILE=overlay-battery.conf
```

- Output: `build/espaxa-zigbee/zephyr/zephyr.uf2`. Flash it by double-tapping
  reset and copying the file to the USB drive that appears; this uses the
  Adafruit UF2 bootloader.
- Console: `/dev/ttyACM0` (USB CDC ACM). Logging starts 4 s after boot.
- Pairing: turn on "Add device" in ZHA; the device joins by itself on first
  boot. For a factory reset, hold D1 (P0.03) at GND while resetting.
- Expected warnings: ZBOSS headers report "`ZB_ZCL_SUPPORT_CLUSTER_*` redefined".
  This is harmless and comes from its own config headers.
- The board target is `xiao_ble`, not `xiao_ble/nrf52840/sense`.

## Layout

| File | Role |
|---|---|
| `src/main.c` | Zigbee glue: clusters, ZCL command callback, signal handler, factory reset pin |
| `src/axa.c`, `src/axa.h` | AXA serial protocol, request queue and the AXA thread |
| `src/motion.c`, `src/motion.h` | Motion state machine: phases, lock state, position estimate, what to send when. Pure C, no Zephyr |
| `src/battery.c`, `src/battery.h` | Battery voltage through the divider on D0, every hour once the AXA has been quiet for a minute |
| `src/battery_level.c`, `src/battery_level.h` | NiMH voltage to percentage curve. Pure C, no Zephyr |
| `tests/` | Host tests from Python: `Makefile` builds `motion.c`, `battery_level.c` and `shim.c` (fake AXA and clock) into a shared library, `harness.py` wraps it with ctypes, `test_*.py` are pytest |
| `HARDWARE.md` | Wiring diagram, AXA connector pinout, XIAO pins, level shifter, battery divider |
| `TESTING.md` | Hardware test plan for when the AXA is connected |
| `boards/xiao_ble.overlay` | AXA UART on `uart0`: D10 TX P1.15 (open drain) and D9 RX P1.14 tied into one bus line, 19200; D8 (P1.13) switches the AXA bus pull-up power; SPI on D8-D10 disabled, Zigbee timer, flash partitions, reset pin |
| `Kconfig` | `CONFIG_ESPAXA_*` timing options and the log module |
| `prj.conf` / `overlay-battery.conf` | Debug default / low-power overlay |
| `sysbuild.conf` | Turns the partition manager off; partitions come from the overlay |

## Architecture

There are two threads, and each one owns its own state:

- **Zigbee (ZBOSS) thread.** Calls `zcl_device_cb()` for the Window Covering
  commands, which only queue requests with `axa_move_to()` / `axa_stop()`.
- **AXA thread** (`axa.c`). Blocks on its request queue with a timeout from
  `motion_next_wake()`, then calls `motion_move_to()` / `motion_stop()` and
  `motion_tick()`. The state machine in `motion.c` runs the blocking serial
  transactions through callbacks.

Communication between them:

- **Zigbee → AXA:** a one-slot `k_msgq`. Only the latest request matters: a
  new request replaces one that has not been picked up yet. ZHA's slider sends
  several GO_TO_LIFT commands per drag; a deeper queue replays them all.
- **AXA → Zigbee:** `on_axa_position()` calls `zigbee_schedule_callback()` to
  run `set_lift_percentage()` in the ZBOSS thread. If scheduling fails the
  callback returns an error, the motion state keeps the old reported value and
  the AXA thread retries every 500 ms. Never call ZBOSS APIs
  directly from the AXA thread. The 0–100 value fits in the callback's
  `zb_uint8_t` parameter.
- `axa_init()` runs after `zigbee_enable()`, so the scheduler exists before the
  first report.
- **Failed commands bounce the position.** A failed OPEN/CLOSE leaves the
  position unchanged, and ZHA would show "opening"/"closing" until its own
  timeout (3 s per percent of travel, up to 5 min). `report_failure()` sends
  one step towards the target and then the real position again, 500 ms apart.
  ZHA reads the second report as a stop and settles on the real state. The
  nudge must go *towards* the target: ZHA only treats a report as movement
  when it lies strictly between the previous value and the target. Reporting
  a nudge at the start of a command instead would break the normal open: ZHA
  expects the next report within 5 s, and the unlock delay is 10 s.

Window Covering mapping:

- Zigbee `CurrentPositionLiftPercentage` counts 0 = fully open and 100 = fully
  closed. The AXA code uses `open_percent`, where 0 = closed, and `main.c`
  inverts it in both directions. ZHA inverts it again for HA.
- Command mapping: `UP_OPEN` → 100, `DOWN_CLOSE` → 0,
  `GO_TO_LIFT_PERCENTAGE` → `100 - value`, `STOP` → stop.
- The app sets the attribute itself; ZBOSS does not move it on commands.
- ZHA's "Inverted" switch writes the reversed motor bit in the Mode
  attribute. The firmware ignores it; it is meaningless for the AXA.

Lock state: a Binary Input cluster (0x000F) on the same endpoint, which ZHA
shows as a binary sensor. `PresentValue` is 0 only after a STATUS reply of
211 Strong Locked, 1 after 210 or 212, and 1 at boot until the first STATUS.
Inverted on purpose: Home Assistant's lock device class reads on as unlocked,
so with "Show as: Lock" it displays correctly. "Reconfigure" in ZHA picks up
a newly added cluster, no need to pair again.
It comes from the raw STATUS replies (`send_cmd()` in `axa.c` calls the lock
callback), not from the lock state in `motion.c`, which counts 212 as locked
and infers states from timers. Only changes are scheduled into the Zigbee
thread.

### AXA protocol (in `axa.c`)

- The line runs at 19200 baud, 8N2. The stop bits are set at runtime with
  `uart_configure`; the overlay only sets the baud rate.
- Each transaction:
  1. Switch on the bus pull-up power (D8), wait 50 ms, resume the UART.
  2. Send `\r` to wake the AXA. Wait at least 50 ms, then drop everything
     until the bus has been quiet for 30 ms (at most 500 ms): an awake AXA
     answers the empty line with `502 Command not implemented`.
  3. Send the command ending in `\r` only, and read lines until one starts
     with 3 digits, within 1 s. Up to 2 retries.
  4. Suspend the UART, switch off D8, then pause 200 ms.
- **`\r` only, never `\r\n`** (found with bus traces, 2026-10-30): the AXA
  answers as soon as it sees `\r`, so a trailing `\n` collides with the start
  of its reply on the half-duplex bus. The UART then loses sync and the reply
  is garbage with framing errors.
- **Bus trace:** the debug build logs every byte sent and received after each
  exchange, with times and UART errors (`trace_dump()` in `axa.c`). It is
  compiled out of the battery build.
- Codes: 200 OK, 210 unlocked, 211 locked, 212 is assumed to mean "closed but
  not fully locked", 260/261 device/firmware info, 502 error.
### Motion state machine (in `motion.c`)

Run the host tests after any change: `cd tests && pytest` (needs a C
compiler, `make` and pytest; `MOTION_LOG=1 pytest -s` prints the state
machine's log). A new field or function the tests need goes through
`tests/shim.c` and the `signatures` dict in `tests/harness.py`.

- **Phases:** idle, unlocking (OPEN accepted while locked, chain starts after
  `UNLOCK_DURATION`), opening, closing, locking (estimate at 0, the AXA locks
  within `LOCK_DURATION`). The position (0.0–1.0) is estimated from time in
  the phase; motion starts at the ack of the motion command.
- **Lock state** is locked, unlocked or unknown, and only STATUS sets it to
  locked. A chain that is out (position > 0) is unlocked. Stopping during
  unlocking, reversing during locking or a lock timer without a 211 make it
  unknown. Opening from 0 with an unknown lock asks STATUS first, to decide
  whether the unlock delay applies.
- **Requests while moving:** same direction only changes the target, no
  commands. Opposite direction sends STOP, then starts the new move.
- **Requests while locking:** Close and Stop are ignored (the window is
  closed, let it lock). Open sends OPEN without STOP and assumes a full
  unlock delay. *Unverified on the AXA*, see `TESTING.md`.
- **Endpoints:** STOP only for intermediate targets; the AXA stops by itself
  at the ends. A failed CLOSE after an accepted OPEN sends STOP.
- **Manual use** (buttons on the AXA): an idle STATUS is compared with the
  previous idle STATUS (the baseline). Every request clears the baseline, so
  the first STATUS after a command never counts as button use. After every
  move a settle STATUS follows within `MOVING_STATUS_INTERVAL`.
  - Locked while not at 0 → closed, whatever the baseline.
  - Unlocked while at 0, baseline locked → fully open.
- **Boot:** assumes closed and locked with a locked baseline, logs DEVICE and
  FIRMWARE, then corrects itself with STATUS: unlocked at boot reads as open.

## Design choices (and why)

- **Zigbee sleepy end device instead of Wi-Fi.** Earlier ESP32 versions with
  deep sleep were slow to react, missed commands, drained batteries and
  reconnected unreliably. With Zigbee, the parent buffers commands for the
  device, and each poll costs a few ms of radio time instead of seconds of
  Wi-Fi.
  - **Estimated budget:** about 1.2–1.7 mAh/day for the electronics, of which
    about 0.8 mAh/day is NiMH self-discharge.
  - Battery life is then limited by the AXA motor and self-discharge.
- **No MQTT.** The user's choice. ZHA needs no broker; Zigbee2MQTT would bring
  MQTT back.
- **Native NCS + ZBOSS instead of ESPHome.** ESPHome's nRF52 Zigbee
  (2026.9) has no UART on that platform and no cover entity. The standard
  Window Covering cluster gives a real cover in ZHA.
- **Add-on v1.4.0 (NCS v3.4.0).** Nordic's docs say v1.3.0 / NCS v3.2.0, but
  the v1.3.0 tag actually pins NCS v2.9.2. v1.4.0 is newer and marked supported.
- **Zigbee poll interval: 5 s** (`CONFIG_ESPAXA_ZIGBEE_POLL_INTERVAL_MS`).
  Parents drop buffered frames after about 7.68 s, so it must stay below that.
  It is set on `ZB_BDB_SIGNAL_DEVICE_REBOOT` / `STEERING` success.
- **UART suspended between transactions** (`CONFIG_PM_DEVICE`). The UARTE
  receiver keeps the HFCLK running, about 0.5 mA, which is more than the radio
  budget. Interrupt-driven RX feeds a byte `k_msgq`; TX uses `uart_poll_out`.
- **STATUS poll every 10 min while idle.** It only exists to detect button use
  on the AXA, and every poll wakes the AXA. While moving it runs every 2 s, to
  catch locking.
- **Closing sends OPEN before CLOSE.** From galagaking/espaxa: the AXA
  blocks at its end position, and a short OPEN releases it. Don't remove it
  without testing.
- **Flash layout.** The XIAO's UF2 bootloader needs the application at 0x27000.
  Zigbee needs `zboss_nvram` (32 kB) and `zboss_product_config` (4 kB) as fixed
  partitions, so the application partition shrinks to 752 kB:
  - `storage` @ 0xe3000
  - `zboss_nvram` @ 0xe5000
  - `zboss_product_config` @ 0xed000
- **ZBOSS header gotchas.**
  - `#define ZB_HA_DEFINE_DEVICE_WINDOW_COVERING` must come before
    `#include <zboss_api.h>`, otherwise the `ZB_HA_DECLARE_WINDOW_COVERING_*`
    macros don't exist.
  - `zb_zcl_basic_attrs_ext_t` comes from `zboss_api_addons.h`.
- **USB console switch.** `CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM` is the switch
  that turns it off. Setting `USB_DEVICE_STACK_NEXT=n` alone is overridden by
  the board defaults.
- **`CONFIG_UART_USE_RUNTIME_CONFIGURE=y` is required.** Nordic's SoC
  defaults turn it off; `uart_configure()` (needed for 8N2) then returns
  `-ENOTSUP`, `axa_init()` fails and the AXA thread never starts.
- **Boot message.** `main()` logs build time, reset cause and timing config.
  The USB console drops everything while no terminal is open, so in the
  console build a delayed work item polls DTR every 500 ms and repeats the
  banner whenever a terminal connects (`CONFIG_UART_LINE_CTRL`, off in the
  battery overlay).
- **All channels.** `CONFIG_ZIGBEE_CHANNEL_SELECTION_MODE_MULTI=y` with the
  default all-channel mask. Nordic defaults to channel 16 only, which never
  finds a ZHA network on another channel.
- **Power Configuration declared by hand.** The Window Covering device macros
  hard-code 5 clusters, so `main.c` declares the cluster list, simple
  descriptor and endpoint itself, with Power Configuration as the 6th.
  ZBOSS pastes the cluster counts and IDs into names: they must be plain
  numbers and names, and `ZB_DECLARE_SIMPLE_DESC` needs a wrapper macro to
  expand them. `ZB_ZCL_DECLARE_POWER_CONFIG_BATTERY_ATTRIB_LIST_EXT` does not
  expand at all; BatteryVoltage gets its own descriptor with reporting.
  Adding the cluster to a paired device needs "Reconfigure" in ZHA.
- **Factory reset pin.** Read once at boot, then set to `GPIO_DISCONNECTED`, so
  a jumper left in place doesn't draw current through the pull-up.
- **Activity LED, debug build only.** The blue LED (alias `activity-led`,
  P0.06) blinks at 2 Hz while the AXA thread is busy: during a serial
  exchange or while the AXA moves. `CONFIG_ESPAXA_ACTIVITY_LED` switches it;
  `overlay-battery.conf` turns it off, since LEDs cost battery.

## Hardware

`HARDWARE.md` has the wiring diagram, the AXA RJ12 pinout and all pins. The
points that matter for the firmware:

- **Board:** Seeed XIAO nRF52840. The bootloader reports a Sense, which the
  plain `xiao_ble` target covers.
- **Power:** the AXA's 4× AA pack (4.0–6.6 V, NiMH or alkaline), tapped at
  the battery bay's + terminal, through an external low-quiescent 3.3 V LDO
  into the 3V3 pin. Not the 5V pin: the XIAO's own regulator (SGM2040, 6 V
  absolute maximum) cannot take fresh alkaline cells. The RJ12 carries
  DATA and GND, and pin 1 gets the battery voltage only during an exchange.
- **Bus pull-up power** (2026-10-30): the AXA's bus pull-up hangs off RJ12
  pin 1, which is dead on batteries; DATA then floats at 1.4 V and the AXA is
  silent. D8 (P1.13) switches battery voltage onto pin 1 through a PNP + NPN
  pair and 1 kΩ, only during an exchange: pin 1 draws 25 to 50 µA when
  powered. 3.3 V on pin 1 is not enough, it only worked on version A.
- **LDO, not a buck converter** (decided 2026-10-21): an estimated 5 to 10%
  standby gain does not justify an inductor and the few parts that take
  6.6 V. Revisit only if the measured sleep current is well above the budget.
  The LDO's own quiescent current matters as much: keep it under 5 µA.
- **Worst-case design:** it has to work on both known AXA board versions
  (bus pull-up 3.9 kΩ or 33 kΩ) without measuring, so the repo can be shared.
  `HARDWARE.md` has the numbers; they are why TX uses high drive
  (`NRF_DRIVE_H0D1`) and why the series resistor is 33 Ω.
- **Data line:** a single-wire bus. TX (D10) and RX (D9) are tied together, and
  that line goes through a single-FET bidirectional level shifter to the AXA
  data line through 33 Ω. The AXA has its own pull-up on the bus; on the
  3.3 V side the internal pull-up of RX is the only one. TX is open drain
  with high drive, so the pull-ups hold the line high and either side can
  pull it low. RX hears every byte the XIAO sends; the
  echoed command line is skipped in `transact()`, and a timeout without any
  echo logs a wiring warning. There is no MCP2003A transceiver and
  no boost converter.
- **Battery sense:** 4.7 MΩ / 2.2 MΩ divider with 100 nF from battery + to
  D0 (AIN0). The XIAO's own battery sense (P0.31) measures the LiPo pads and
  cannot see the AXA's pack. Below 0.5 V per cell the firmware assumes no
  divider is fitted and reports the battery as unknown.
- **Open questions:** listed at the end of `HARDWARE.md`.

## Reference

- Original ESPHome version with the same AXA logic, in C++: `~/git/espaxa`, at
  `components/espaxa/`.
- ZBOSS sources:
  - `~/ncs/zigbee-v1.4.0/ncs-zigbee/lib/zboss/include/zcl/zb_zcl_window_covering.h`
  - `.../include/ha/zb_ha_window_covering.h`
  - `.../src/zcl/zcl_window_covering.c`
- Nordic's samples to compare against: `~/ncs/zigbee-v1.4.0/ncs-zigbee/samples/`.
  `light_switch` shows the sleepy end-device setup, `light_bulb` the ZCL
  device callback.

## Next steps

- Bring-up on hardware: AXA serial exchange, joining ZHA, and the direction of
  the cover in HA.
- Measure the sleep current of the battery build.
- Calibrate the battery curve in `battery_level.c` against the real pack.
- Rejoin by itself after more than 200 s without a network (see `TESTING.md`
  8.4).
