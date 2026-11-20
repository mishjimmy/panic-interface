# panic-interface

Firmware for a **LILYGO T-RSS3** (ESP32-S3). It watches a dry contact and reports its status as text over the board's isolated RS232 and/or RS485 port.

- Contact **closed** = normal → `not_in_emergency`
- Contact **open** = emergency → `emergency_active`

A cut wire reads as open, so it **fails safe into emergency**.

## Wiring

| What | Board connection |
|---|---|
| Dry contact, side 1 | **GPIO6**, header J4 pin 5 |
| Dry contact, side 2 | **GND**, header J4 pin 6 or 7, next to pin 5 |
| RS232 out | DB9 connector: pin 3 = TXD (board → device), pin 2 = RXD, pin 5 = SGND |
| RS485 out | 3-pin 5.08 mm terminal: **A**, **B**, **SGND** |
| Power | USB-C, or 7–24 V DC on the 2-pin terminal |

The contact pin uses the ESP32's internal pull-up, so no external resistor is needed. For long contact runs, use twisted pair and consider an RC filter at the board.

The RS232 and RS485 sides are galvanically isolated (RSM232MT / RSM485MT5V modules). Connect SGND to the remote device's signal ground. Terminate the RS485 bus (120 Ω across A/B) at its ends if the cable is long.

### Pins used and where they came from

`github.com/Xinyuan-LilyGO/T-RSS3` does not exist. LilyGO publishes T-RSS3 files in **[Xinyuan-LilyGO/T-RSC3](https://github.com/Xinyuan-LilyGO/T-RSC3)** ("LILYGO T-RSC3 & RSS3"). The pins below come from `example/RS485_simple/utilities.h` (`CONFIG_IDF_TARGET_ESP32S3` branch) and are confirmed in `schematic/T-RSS3.pdf`:

| Function | GPIO |
|---|---|
| RS232 TX / RX | 41 / 42 |
| RS485 TX / RX | 3 / 2 |
| RS485 direction (`CON`: HIGH = transmit, LOW = receive) | 4 |
| WS2812 status LED | 1 |
| BOOT button | 0 |
| User button S3 (optional footprint) | **5** |

> GPIO2 is **RS485 RX**, not the user button. The user button is on GPIO5.

RS485 **does** need direction control. The firmware sets CON HIGH, writes, calls `flush()`, then sets CON LOW, and the line idles in receive. `flush()` blocks for the time it takes to send one line (about 19 ms at 9600 baud).

The firmware uses UART1 for RS232 and UART2 for RS485, not UART0, so boot or core log output can never appear on the RS232 line.

## Configuration

Everything you can adjust is in the block at the top of [src/main.cpp](src/main.cpp):

| Setting | Default | Meaning |
|---|---|---|
| `NORMAL_INTERVAL_S` | 60 | Repeat rate in NORMAL (seconds) |
| `EMERG_FAST_INTERVAL_S` | 1 | Repeat rate in EMERGENCY_FAST (seconds) |
| `EMERG_FAST_DURATION_S` | 300 | How long EMERGENCY_FAST lasts before EMERGENCY_SLOW (seconds) |
| `EMERG_SLOW_INTERVAL_S` | 60 | Repeat rate in EMERGENCY_SLOW (seconds) |
| `RECOVERY_INTERVAL_S` | 1 | Repeat rate in RECOVERY (seconds) |
| `RECOVERY_DURATION_S` | 60 | How long RECOVERY lasts before NORMAL (seconds) |
| `CLEAR_CONFIRM_S` | 5 | How long the contact must stay closed before emergency ends (seconds) |
| `OPEN_DEBOUNCE_MS` | 50 | How long the contact must be open before emergency triggers (**milliseconds**) |
| `CONTACT_PIN` | 6 | Contact input GPIO |
| `MSG_NORMAL` / `MSG_EMERGENCY` | `not_in_emergency` / `emergency_active` | Message text |
| `LINE_TERMINATOR` | `"\r\n"` | Appended to every message |
| `BAUD_RATE` | 9600 | RS232/RS485 baud |
| `SERIAL_CONFIG` | `SERIAL_8N1` | Data bits, parity and stop bits |
| `OUTPUT_PORT` | `PORT_RS232` | `PORT_RS232`, `PORT_RS485` or `PORT_BOTH` |
| `ENABLE_STATUS_LED` | 1 | 0 turns the WS2812 off |
| `LED_BRIGHTNESS` | 40 | LED brightness cap (0–255) |

## State machine

```
                 boot, contact closed                 boot, contact open
                         │                                   │
                         ▼                                   ▼
               ┌───────────────────┐   open ≥50 ms   ┌───────────────────┐
     ┌────────►│      NORMAL       ├────────────────►│  EMERGENCY_FAST   │◄──────────┐
     │         │ not_in_emergency  │                 │ emergency_active  │           │
     │         │   every 60 s      │                 │ every 1 s, 5 min  │           │
     │         └───────────────────┘                 └───┬──────────┬────┘           │
     │                                       5 min done  │          │ closed ≥5 s    │
     │                                                   ▼          │                │
     │                                   ┌───────────────────┐      │                │
     │                                   │  EMERGENCY_SLOW   │      │                │
     │                                   │ emergency_active  │      │                │
     │                                   │   every 60 s      │      │                │
     │                                   └─────────┬─────────┘      │                │
     │                                closed ≥5 s  │                │                │
     │                                             ▼                ▼                │
     │                                   ┌──────────────────────────────┐  open      │
     │         1 min done                │           RECOVERY           │  ≥50 ms    │
     └───────────────────────────────────┤      not_in_emergency        ├────────────┘
                                         │      every 1 s, for 1 min    │
                                         └──────────────────────────────┘
```

- The first message is sent **as soon as** the board enters any state. After that it repeats at that state's interval.
- If the contact reopens before `CLEAR_CONFIRM_MS` runs out, the board stays in its current emergency state. Timers don't reset and nothing extra is sent.
- If the contact reopens during RECOVERY, the board goes back to EMERGENCY_FAST with a fresh 5-minute window.
- The code is non-blocking: there is no `delay()` in `loop()`, and all timing uses unsigned `millis()` subtraction, so the 49-day rollover is safe.

### Status LED

| State | LED |
|---|---|
| NORMAL | Green, slow breathing |
| EMERGENCY_FAST | Red, fast flash (5 Hz) |
| EMERGENCY_SLOW | Red, short blink every 2 s |
| RECOVERY | Solid amber |

## Build, flash, monitor

```sh
pio run                      # build
pio run -t upload            # flash over USB-C
pio device monitor           # USB debug console
```

If `pio` isn't on your PATH after a pip install, use `python -m platformio` instead.

The USB console shows every message sent and every state change with a `millis()` timestamp, for example:

```
[t=812 ms] BOOT contact=CLOSED port=RS232 baud=9600
[t=812 ms] STATE NORMAL -> NORMAL
[t=812 ms] TX(RS232): not_in_emergency
[t=15403 ms] STATE NORMAL -> EMERGENCY_FAST
[t=15403 ms] TX(RS232): emergency_active
```

Uploads go through the chip's ROM loader (`upload_flags = --no-stub`, 115200 baud). With esptool 4.5.1, the usual flasher stub drops the native USB link on this board and fails with "No serial data received". A full flash still takes about 15 seconds.

If the board doesn't enter download mode, hold **BOOT**, tap **RST**, then release BOOT.
