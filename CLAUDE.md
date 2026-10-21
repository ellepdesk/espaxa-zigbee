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
| `tests/test_motion.c`, `tests/run.sh` | Host tests for the state machine against a fake AXA and clock |
| `TESTING.md` | Hardware test plan for when the AXA is connected |
| `boards/xiao_ble.overlay` | AXA UART on `uart0`: D6 TX P1.11 (open drain) and D5 RX P0.05 tied into one bus line, 19200; I2C on D4/D5 disabled, Zigbee timer, flash partitions, reset pin |
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

### AXA protocol (in `axa.c`)

- The line runs at 19200 baud, 8N2. The stop bits are set at runtime with
  `uart_configure`; the overlay only sets the baud rate.
- Each transaction:
  1. Resume the UART.
  2. Send `\r\n` to wake the AXA, wait 50 ms, then purge what it replied.
  3. Send the command and read lines until one starts with 3 digits, within 1 s.
     Up to 2 retries.
  4. Suspend the UART, then pause 200 ms.
- Codes: 200 OK, 210 unlocked, 211 locked, 212 is assumed to mean "closed but
  not fully locked", 260/261 device/firmware info, 502 error.
### Motion state machine (in `motion.c`)

Run the host tests after any change: `tests/run.sh` (needs only a C compiler;
`MOTION_LOG=1 tests/run.sh` prints the state machine's log).

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
- **Closing sends OPEN before CLOSE.** Carried over from the original
  implementation, which found it necessary. The reason is unknown; don't remove
  it without testing.
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
- **Factory reset pin.** Read once at boot, then set to `GPIO_DISCONNECTED`, so
  a jumper left in place doesn't draw current through the pull-up.
- **Activity LED, debug build only.** The blue LED (alias `activity-led`,
  P0.06) blinks at 2 Hz while the AXA thread is busy: during a serial
  exchange or while the AXA moves. `CONFIG_ESPAXA_ACTIVITY_LED` switches it;
  `overlay-battery.conf` turns it off, since LEDs cost battery.

## Hardware

- **Board:** Seeed XIAO nRF52840.
- **Power:** a 4× NiMH AA pack (4.0–5.8 V) on the 5V pin, shared with the AXA,
  which runs on its own 4 AA cells.
- **Data line:** a single-wire bus. TX (D6) and RX (D5) are tied together, and
  that line goes through a single-FET bidirectional level shifter to the AXA
  data line. TX is open drain, so the shifter's pull-ups hold the line high
  and either side can pull it low. RX hears every byte the XIAO sends; the
  echoed command line is skipped in `transact()`, and a timeout without any
  echo logs a wiring warning. There is no MCP2003A LIN transceiver and
  no boost converter.
- **Open question:** the maximum input voltage of the XIAO's 5V-pin regulator
  is unverified against freshly charged NiMH cells (about 5.8 V). A series
  diode is the fallback.
- **Unknown AXA figures:** standby current and the charge used per open/close
  cycle. Both need measuring.

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
- Add a battery level report (Power Configuration cluster).
