# Hardware test plan

Bring-up and acceptance tests for the first time the XIAO is wired to a real
AXA Remote 2.0. Work through the sections in order: each one depends on the
previous one passing.

The state machine logic is covered by the host tests (`cd tests && pytest`). This
plan covers what the host tests cannot: the AXA's real replies and timings,
the wiring, Home Assistant, and a few AXA behaviours the firmware currently
*assumes*. Those are marked **Unverified** and are the most important results
to bring back.

## Setup

1. Build and flash the debug firmware with the short STATUS poll:

   ```sh
   cd ~/ncs/zigbee-v1.4.0 && source .venv/bin/activate
   export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
   west build -b xiao_ble --sysbuild -d ~/git/espaxa-zigbee/build ~/git/espaxa-zigbee -p always \
     -- -DEXTRA_CONF_FILE=overlay-test.conf
   ```

   Double-tap reset and copy `build/espaxa-zigbee/zephyr/zephyr.uf2` to the
   XIAO-SENSE drive.

2. Keep a console log of every session. The boot banner is repeated when the
   terminal connects, so the log always starts with the build time:

   ```sh
   picocom -b 115200 --logfile axa-$(date +%F-%H%M).log /dev/ttyACM0
   ```

3. Have the cover open in Home Assistant, and a stopwatch at hand.
4. Start with the window closed and locked, using the AXA's own button.

Record for each test: pass or fail, and the relevant console lines.

## 1. Serial link

| # | Step | Expected |
|---|---|---|
| 1.1 | Reset the XIAO with the AXA connected | `Device: ...` and `Firmware: ...` lines, no "No response" retries. The echo of each command on the shared bus is skipped silently |
| 1.2 | Watch the first STATUS | `STATUS -> 211 '211 ...'` with the window locked |
| 1.3 | Note the exact reply text of DEVICE, FIRMWARE and STATUS | Copy them into this file, they document the protocol |
| 1.4 | Note which bus pull-up the AXA has: the resistor marked 392 or 333, see `HARDWARE.md` | Recorded in the results table, so both board versions get tested over time |
| 1.5 | Scope DATA at the RJ12 during a STATUS exchange | Idles at the battery voltage. Low under 0.25 V while the XIAO sends, under 0.3 V while the AXA answers. Rising edges reach the high level within 15 µs |

If every command times out:

- Check that D9 and D10 are tied together, and that the common line goes to
  the 3.3 V side of the level shifter.
- `No echo of STATUS on the bus` means the XIAO does not even hear itself:
  the bus line is broken, shorted to ground, or D9 and D10 are not tied.
- Timeouts without that warning mean the bus works but the AXA does not
  answer. Check the AXA side of the shifter.
- Check that the level shifter's gate is on 3V3. Its 3.3 V side relies on
  the internal pull-up of RX, its high side on the AXA's own pull-up: the bus
  should idle high with the XIAO disconnected.
- Lines that show up as `Skipping '...'` mean the link works but the replies
  look different from what the firmware expects. Capture them.

## 2. Timings

Measure each one three times with the stopwatch, starting at the
`OPEN -> 200` or `CLOSE -> 200` log line. Then put the averages into the
`Kconfig` defaults. They drive the whole position estimate.

| # | Measure | Kconfig option | Default |
|---|---|---|---|
| 2.1 | Open from locked: from the ack until the chain starts moving | `ESPAXA_UNLOCK_DURATION_MS` | 10 s |
| 2.2 | Chain travel from closed to fully open | `ESPAXA_OPEN_DURATION_MS` | 50 s |
| 2.3 | Close: from the chain fully in until STATUS reports 211 | `ESPAXA_LOCK_DURATION_MS` | 30 s |
| 2.4 | Chain travel from fully open to closed | Compare with 2.2 | — |

If 2.2 and 2.4 differ by more than about 10%, closing needs its own duration.
That is a firmware change to make.

## 3. Basic commands from Home Assistant

Each command reaches the board within 5 s, the Zigbee poll interval.

| # | Step | Expected |
|---|---|---|
| 3.1 | Open | Unlocks, opens fully, no STOP in the log. HA shows "opening" and then "open" at 100% |
| 3.2 | Close | Log shows OPEN, then CLOSE, then the window closes and locks. HA ends at "closed" at 0% |
| 3.3 | Set the position to 50% | STOP when the estimate reaches 50%. Measure the actual opening against fully open |
| 3.4 | Set it to 80% from 50% | Opens further and stops. No unlock delay, because the chain is already out |
| 3.5 | Set it to 20% from 80% | STOP, OPEN, CLOSE in the log, then a STOP at 20% |
| 3.6 | Stop while opening | Stops. HA position matches the estimate in the log |
| 3.7 | Compare the HA direction | The HA "open" button opens the window. If it is reversed, the inversion in `main.c` is wrong |

Write down the position error of 3.3 to 3.5. A consistent error points to
the timings in section 2.

## 4. Unverified AXA behaviour

The firmware makes assumptions in these cases. Each result decides whether
`motion.c` keeps its current behaviour.

| # | Step | What to find out | Firmware assumes |
|---|---|---|---|
| 4.1 | Close, then press Stop in HA while it is locking, after the chain is in and before 211 | Nothing should be sent. Does the window still lock? | Stop is ignored while locking |
| 4.2 | Close, then press Open in HA while it is locking | What does the AXA do with an OPEN mid-lock: unlock and open, ignore it, or error? How long until the chain moves? | OPEN without STOP, full unlock delay |
| 4.3 | Close, then press Close again while it is locking | Nothing should be sent, and the window locks | Repeated Close is ignored |
| 4.4 | Open from locked, then Stop within the first 5 s | What does STATUS say afterwards? Does the next Open still have an unlock delay? | Lock unknown, STATUS decides |
| 4.5 | Send STATUS while unlocking and while moving, from the log of 3.1 and 3.2 | Which code does it return during each phase? | 210 while moving |
| 4.6 | Find a situation that gives 212, for example a blocked lock | When does the AXA report 212? | 212 means closed but not fully locked |
| 4.7 | Temporarily remove the OPEN before CLOSE in `start_close()` and close from 50% | Does a bare CLOSE work? | OPEN first is needed, carried over from the ESPHome version |

## 5. State machine on real hardware

These repeat the host tests against the real AXA.

| # | Step | Expected |
|---|---|---|
| 5.1 | Open from locked, then Stop during the unlock delay. Wait for 2 STATUS polls | Position stays 0% and no "Window was opened manually" |
| 5.2 | Drag the HA slider in one motion from 0% to 70% | One OPEN only, then a STOP at 70%. No back-and-forth |
| 5.3 | While opening to 80%, drag the slider to 90% | No new commands, stops at 90% |
| 5.4 | While opening, drag the slider below the current position | STOP, then OPEN and CLOSE |
| 5.5 | Close fully and wait | `Locked` in the log before the lock timer runs out, so the 211 confirmed it |

## 6. Buttons on the AXA

With `overlay-test.conf`, idle STATUS polls come every 30 s.

| # | Step | Expected within 30 s |
|---|---|---|
| 6.1 | Window closed and locked: open it with the AXA button | "Window was opened manually", HA shows 100% |
| 6.2 | Window open: close it with the AXA button and let it lock | "Window was closed manually", HA shows 0% |
| 6.3 | Window at 50% from HA: close it with the AXA button | Closed in HA after the lock |
| 6.4 | Open with the button and stop half way with the button | **Known limit:** HA shows 100%. The AXA only reports locked or unlocked |

## 7. Failure handling

| # | Step | Expected |
|---|---|---|
| 7.1 | Disconnect the AXA data line and press Open in HA | `OPEN failed (-1)`. HA shows "opening" for about 4 s, then back to the old state |
| 7.2 | Reconnect it and press Open | Works normally |
| 7.3 | Disconnect the data line during a close from 50%, before CLOSE is sent. This needs timing, so retry if it misses | `CLOSE failed` followed by STOP. Check that the AXA did not keep opening |

## 8. Zigbee

| # | Step | Expected |
|---|---|---|
| 8.1 | Command latency: press Open in HA 10 times and note the delay until the log shows it | All within 5 s |
| 8.2 | Restart Home Assistant | The cover comes back with the right position |
| 8.3 | Power off the coordinator for 1 minute | The device rejoins by itself |
| 8.4 | Power off the coordinator for 5 minutes | **Known issue:** after 200 s the device stops looking and needs a reset. Confirm it, the fix is pending |

## 9. Battery build

Run this only after sections 1 to 8 pass. Section 10 can run before or after
it.

1. Build without the overlay and with `overlay-battery.conf`, as in the README.
2. Flash it and power the board from the battery pack through a current
   meter in front of the LDO, with USB disconnected.
3. Measure the idle current over at least 10 minutes, so it includes a STATUS
   poll. The target is the budget in `CLAUDE.md`, about 1.2 to 1.7 mAh/day.
4. Repeat 3.1 and 3.2 to confirm commands still work without the console.

## 10. Battery level

Needs the divider on D0 from `HARDWARE.md`. The first measurement comes 20 s
after boot at the earliest, then every hour. Each one waits until the AXA has
been quiet for a minute.

| # | Step | Expected |
|---|---|---|
| 10.1 | After pairing, or after "Reconfigure" in ZHA for an already paired device | A battery sensor appears on the device in HA |
| 10.2 | Boot with the console open and wait about 90 s | `Battery ... mV, ...%`. Compare the mV with a meter on the battery bay's + terminal: within 2% |
| 10.3 | Boot without the divider fitted | `no divider on D0?` warning, HA shows the battery as unknown |
| 10.4 | Open and close the window, then watch the next measurement | It waits until 60 s after the AXA went quiet |
| 10.5 | Over the life of a set of cells, note the reported percentage and the meter voltage every week | Data to calibrate the curve in `src/battery_level.c` |

For quicker checks, build with `CONFIG_ESPAXA_BATTERY_INTERVAL_MS=60000` in
`overlay-test.conf`.

## 11. Strong Locked sensor

Press "Reconfigure" on the device in ZHA first, so it sees the Binary Input
cluster, and set the new sensor's "Show as" to Lock. On = unlocked.

| # | Step | Expected |
|---|---|---|
| 11.1 | After the reconfigure, with the window locked | A lock sensor on the device, Locked after the first STATUS. Log: `Strong Locked` |
| 11.2 | Open from HA | Shows Unlocked within one moving STATUS poll (2 s) of the AXA unlocking. Log: `Not Strong Locked` |
| 11.3 | Close from HA | Shows Locked with the STATUS that returns 211, at about the same time as `Locked` |
| 11.4 | If 212 ever shows up (see 4.6) | The sensor stays Unlocked |

## Results

| Date | Firmware build time | AXA pull-up (392 or 333) | Cells | Sections run | Notes |
|---|---|---|---|---|---|
| | | | | | |
