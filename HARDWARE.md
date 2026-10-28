# Hardware

A Seeed XIAO nRF52840 talks to the AXA Remote 2.0 over its single-wire data
line, through a one-FET level shifter. It is powered from the AXA's own
battery pack through a small external 3.3 V regulator, and measures the pack
voltage through a high-impedance divider. The RJ12 carries only data and
ground.

The design is meant to work on every AXA board version and with any 4 AA
cells, without measuring anything first. The worst-case figures are in
[Worst case](#worst-case).

```
 AXA battery bay                                Seeed XIAO nRF52840

 + terminal ──┬── LDO 3.3 V ─────────────────── 3V3
              │   (HT7333-A)
              │
              └── 4.7 MΩ ─┬──────────────────── D0  (AIN0)
                          ├── 2.2 MΩ ── GND
                          └── 100 nF ── GND

                                         gate ── 3V3
 AXA RJ12 6P6C               ┌──────────┴──┐
 DATA (3, 4) ──── 33 Ω ──────┤ D   BSS138 S├──┬── D5  (RX, internal pull-up)
 (pulled up in the AXA)      └─────────────┘  └── D6  (TX, open drain)
 GND  (2, 5) ────────────────────────────────── GND
 Vcc  (1, 6)   not connected                    5V  not connected
```

## Parts

| Part | Value | Notes |
|---|---|---|
| Seeed XIAO nRF52840 | Plain or Sense | |
| LDO | 3.3 V, input rated 7 V or more, quiescent current under 5 µA | For example HT7333-A (12 V, about 4 µA). Input and output capacitors as its datasheet asks |
| N-channel MOSFET | BSS138 or similar | Gate threshold under 1.5 V, logic level |
| Series resistor | 33 Ω | Between the drain and DATA |
| Divider, top | 4.7 MΩ | Battery + to D0 |
| Divider, bottom | 2.2 MΩ | D0 to GND |
| Capacitor | 100 nF | D0 to GND |

No pull-up resistors are needed: the AXA pulls DATA up, and the internal
pull-up of RX does the low-voltage side.

## AXA connector

The AXA has an RJ12 6P6C socket. The pinout comes from the schematic of the
ESPHome version (`~/git/espaxa/docs/`):

| RJ12 pin | Signal |
|---|---|
| 1, 6 | Vcc. Not connected here; the ESPHome version fed its 8 V supply in on these |
| 2, 5 | GND |
| 3, 4 | DATA |

DATA is a single-wire bus with its pull-up inside the AXA: it idles high,
and either side pulls it low to send. It carries plain serial data. The
ESPHome version used an MCP2003A transceiver, a boost converter to 8 V and a
4.7 kΩ pull-up to 8 V. This design has none of these.

The two RJ12 sockets of the AXA are wired straight through to each other,
power included. Never combine a supply on the RJ12 with a controller fed from
the battery bay.

## Inside the AXA

From opening several units, described in the
[Tweakers thread](https://gathering.tweakers.net/forum/list_messages/1991748)
(posts by elleP and Stainless Steel, November and December 2022):

- **Bus pull-up:** a resistor from the AXA's supply to DATA. The bus therefore
  idles at the battery voltage.
- **Bus driver:** the AXA's microcontroller runs at 3.3 V and pulls DATA low
  through a transistor and a small series resistor.
- **Board versions:** the resistor values differ between PCB versions. The
  transistors differ too, but that does not matter for the bus.

| | Pull-up, supply to DATA | Series resistor, AXA driver to DATA |
|---|---|---|
| Version A | 3.9 kΩ, marked 392 | 33 Ω, marked 330 |
| Version B | 33 kΩ, marked 333 | 100 Ω, marked 01A |

- **Driving it directly at 3.3 V:** with only a 33 Ω resistor, this worked on
  version A. On version B the AXA did not respond. The likely cause is its
  pull-up, about 10 times weaker, which leaves the levels marginal. The level
  shifter in this design drives the bus to its full swing on both versions.
- **One FET is enough:** the thread's level shifter is the one used here:
  gate on 3V3, RX and TX on the source, DATA on the drain. The AXA's pull-up
  serves the high side and the internal pull-up of RX the low side. Another
  user confirmed that OPEN and CLOSE only worked reliably with the MOSFET in
  place.

## XIAO pins

| XIAO pin | nRF52840 | Use |
|---|---|---|
| 3V3 | — | Power in, from the external LDO |
| 5V | — | Not connected. USB only, for flashing and the debug console |
| GND | — | AXA GND, RJ12 pins 2 and 5 |
| D0 | P0.02, AIN0 | Battery voltage, middle of the divider |
| D1 | P0.03 | Factory reset: connect to GND while resetting |
| D5 | P0.05 | AXA bus RX, tied to D6 |
| D6 | P1.11 | AXA bus TX, open drain with high drive, tied to D5 |
| Blue LED | P0.06 | Activity LED, debug build only |

Free pins: D2, D3, D4, D7, D8, D9 and D10. D2, D3 and D4 can also be analog
inputs. The board's I2C bus on D4 and D5 is disabled, because D5 is the bus RX.

The bootloader may identify the board as a XIAO nRF52840 Sense. The firmware
uses the plain `xiao_ble` target, which covers both; the Sense's IMU and
microphone are not used.

## Data line

- **D5 and D6 tied together:** the UART's RX and TX share the single bus line,
  which goes to the source of the FET.
- **TX is open drain with high drive:** it only pulls low, with at least 6 mA
  at 0.4 V. The pull-ups hold the line high, so the AXA can pull it low while
  the XIAO is idle. A push-pull TX would fight the AXA's replies.
- **RX hears our own transmissions:** the firmware skips the echo of each
  command. A timeout without an echo logs a wiring warning, see `TESTING.md`.
- **Level shifter:** a single N-channel MOSFET with its gate on 3V3. The AXA's
  own pull-up holds the drain side high. The internal pull-up of RX on D5,
  11 to 16 kΩ, holds the source side high; an external one is not needed.
- **Series resistor:** 33 Ω between the drain and DATA, as the AXA's own
  driver has. It keeps the low level within margin, see below, and limits the
  current if DATA is ever shorted to the battery by a wiring mistake.
- **Wire length:** keep the wires to the AXA short, inside or at the battery
  bay. The 33 kΩ pull-up of version B makes the bus slow beyond about 300 pF,
  a few metres of cable.
- **Line settings:** 19200 baud, 8 data bits, no parity, 2 stop bits.
- **Between transactions:** the UART is suspended and both pins switch to
  their low-power state. The source side then floats to about 3.3 V minus the
  FET's threshold, where the FET stops conducting, and the AXA's pull-up keeps
  the bus idle.

## Battery voltage

The XIAO's own battery sense on P0.31 measures the LiPo pads, so it cannot
see the AXA's pack. An external divider from the battery's + terminal to D0
measures it:

- **Range:** 4 fresh alkaline cells at 6.6 V give 2.1 V on D0. The ADC uses
  its internal 0.6 V reference with gain 1/6, which gives a 3.6 V range.
- **Current:** under 1 µA, about 0.02 mAh per day.
- **Why the capacitor:** the divider is too high-impedance for the ADC's
  40 µs sampling time. The capacitor holds the voltage and supplies the
  sampling charge.
- **Without the divider:** the reading is near 0 V and the firmware reports
  the battery as unknown.

The firmware measures every hour, and only after the AXA has been quiet for a
minute, because the motor's load makes the pack sag. It reports the voltage
and an estimated percentage in the Zigbee Power Configuration cluster. The
percentage follows a typical NiMH discharge curve and needs calibrating
against the real pack, see `TESTING.md`. With alkaline cells the voltage is
right, but the percentage reads 100% until the cells drop below 1.38 V each.

## Power

- **Supply:** the AXA's 4 AA cells, tapped at the battery bay's + terminal,
  through the external LDO into the XIAO's 3V3 pin. Ground is common with the
  AXA through RJ12 pins 2 and 5.
- **Why not the 5V pin:** behind it sit a diode and the XIAO's own regulator,
  an SGM2040 rated for 5.5 V with 6 V absolute maximum. Seeed has since
  changed that part without naming the new one. 4 fresh alkaline cells reach
  6.6 V, about 6.3 V after the diode, which is over the maximum. The 5V pin
  also powers the board's LiPo charger, which then draws current for nothing.
- **LDO, not a buck converter:** the nRF52840 already runs its internal DC/DC
  converter for its core, and the XIAO sleeps most of the time, where bucks
  are least efficient. A buck would save an estimated 0.1 to 0.2 mAh per day,
  5 to 10% of the standby drain, at the cost of an inductor, layout care and
  few suitable parts above 6.6 V input. Revisit only if the measured sleep
  current (`TESTING.md` section 9) is well above the estimate.
- **USB:** for flashing and the debug console USB can stay connected while
  the LDO is powered. Both regulators then feed 3V3 at the same voltage,
  which is fine for debugging.
- **Budget:** about 1.2 to 1.7 mAh per day for the electronics, of which
  about 0.8 mAh per day is self-discharge of NiMH cells. The motor and
  self-discharge then limit battery life.
- **Battery build:** no USB, console, logging or LED.

## Worst case

Across both AXA board versions and 4 AA cells of any kind:

| Condition | Value |
|---|---|
| Battery voltage | 4.0 V (empty) to 6.6 V (fresh alkaline) |
| AXA pull-up | 3.7 kΩ (3.9 kΩ −5%) to 35 kΩ (33 kΩ +5%) |
| RX pull-up | 11 to 16 kΩ |
| Bus capacitance | up to 300 pF |

**The XIAO pulls the bus low.** The pin sinks the AXA's pull-up current
through the FET and the series resistor, plus the RX pull-up current.

| | Worst case |
|---|---|
| Current, 6.6 V into 3.7 kΩ, plus 3.3 V into 11 kΩ | 2.1 mA |
| Pin low level, high drive (6 mA at 0.4 V guaranteed) | 0.14 V |
| FET, about 10 Ω at 3 V gate drive | 0.02 V |
| 33 Ω series resistor | 0.06 V |
| Bus low level | 0.22 V |

Standard drive only guarantees 1 mA at 0.4 V, less than the 2.1 mA needed,
so the firmware sets TX to high drive. With 100 Ω instead of 33 Ω the low
level would be 0.34 V, still fine but with less margin against the AXA's
input threshold, which is unknown.

**The AXA pulls the bus low.** It sinks its own pull-up current plus the RX
pull-up current, through its series resistor: 2.1 mA through 33 Ω on version
A, 0.5 mA through 100 Ω on version B. Both come to under 0.1 V plus its
transistor. The XIAO's RX sees that plus the FET and the 33 Ω, well under its
0.3 × VDD input threshold of about 1 V.

**Rising edges.** On version B the AXA's 35 kΩ pull-up charges up to 300 pF:
a time constant of 10 µs, against a bit time of 52 µs. The UART samples in the
middle of the bit, so this still leaves margin. The XIAO's side rises faster:
once the FET stops conducting, its own pull-up takes over.

**High levels.** The bus idles at the battery voltage, 4.0 V or more. The
XIAO's side idles at 3.3 V through its RX pull-up.

## Open questions

- **AXA figures:** its standby current and the charge per open and close
  cycle still need measuring.
- **AXA input threshold:** unknown. Both board versions should see under
  0.25 V as low, which a scope check during bring-up confirms (`TESTING.md`).
