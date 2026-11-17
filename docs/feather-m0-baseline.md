# W8 deployment record, Nov 9–14 (git tag `feather-m0-w8`)

This is a record of the system as deployed outdoors on the first sensor board, taken on 2026-11-15. The current
system (W9, new sensor board) is described in the README and on the dashboard's reference page. To rebuild it: `git checkout feather-m0-w8 && ./build.sh`.

## Hardware

| Item | Detail |
|---|---|
| MCU | Adafruit Feather M0 (SAMD21G18, 48 MHz), FQBN `adafruit:samd:adafruit_feather_m0` |
| Radio | Reyax RYLR998 (LoRa, 915 MHz) on Serial1 (SERCOM0, D0 RX / D1 TX), 115200 baud |
| Barometer | BMP180 on I2C (SDA/SCL), address 0x77, OSS 3 |
| Anemometer | 3 cups, R ≈ 185 mm, reed switch with 2 closures per revolution; 608CE ceramic bearings (rough after reassembly) |
| Rain gauge | Tipping bucket reed, 0.369 mm per tip |
| Battery | LiPo on the Feather's JST connector, measured through the on-board 1:2 divider |
| Clock | 32.768 kHz crystal (conformal-coated) drives the RTC; falls back to the internal OSC32K if it fails |
| Receiver | A second Feather M0 + RYLR998 on the host's USB (same sketch with `SENSOR=0`) |

### Sensor pins

| Pin | Use |
|---|---|
| D5 (PA15, EIC) | Wind reed to GND, internal pull-up, active low |
| D10 (PA18, EXTINT2) | Rain reed to GND, internal pull-up; ~100 ms closure per tip |
| D6 | RYLR998 NRST (active low) |
| D0 / D1 | RYLR998 TXD / RXD (Serial1) |
| A7 = D9 | Battery voltage through the 1:2 divider (D9 is not used otherwise) |
| SDA / SCL | BMP180 |

## Radio link

The radio is set with `AT+MODE=0`, `AT+NETWORKID=18`, `AT+BAND=915000000`, `AT+PARAMETER=7,9,1,12` (SF7, BW 500 kHz,
CR 4/5, preamble 12) and `AT+CRFOP=2` (2 dBm). The sensor is address 1 and the receiver address 2.
TX goes by DMA, because the RYLR998 splits a command at any idle gap of about one character.

## Firmware: W8-uart1

The sketch is `link/link.ino`. The portable logic is in `PulseCounter.h`, `TipCounter.h`, `ReportSchedule.h` and
`Bmp180.h`, which are host-tested by `test_*.cpp`. `RadioUart.h` and `Diagnostics.h` are SAMD-specific.

- **Timebase:** 32768 ticks/s from the RTC. Reed edges are debounced for 10 ms (328 ticks).
- **Raw edge counters:** these count every edge before the debounce. They exposed the reed-line chatter on Nov 13.
- **Power modes:**
  - Deep standby on battery: SLEEPDEEP, with RTC, EIC and WDT running.
  - Light sleep when a USB host is seen at boot.
- **Watchdog:** the WDT has an early warning. The firmware detects a dead crystal and fails over to the internal oscillator.
- **Payload:** W8, 22 lowercase-hex fields, worst case 223 bytes against the radio's 240-byte limit (layout in `link.ino`).

## Measured performance (Nov 9–13)

| Metric | Value |
|---|---|
| Battery drain, W8 deep standby | ≈ 0.9 mV/h at 4.18–4.20 V (W7 light sleep was ≈ 8 mV/h) |
| Packet loss | 0.18% outdoors (mounted, RSSI ≈ −93 dBm, SNR ≈ +5…+10) |
| Bearing friction a (spin-down) | 608CE bench 0.0032 rev/s²; as mounted, rebalanced 0.0056 rev/s² |
| Wind calibration | Dashboard uses 6.5 kn per rev/s; field points: 2 kt → 0.21–0.22 rev/s, 4 kt → 0.43 rev/s, 6 kt → 0.93 rev/s (inconsistent above 4 kt) |

## State when bookmarked

- **Takedown:** maintenance window 6 has been open since 2026-11-13 16:20:06, for reed-line chatter that started after
  the Nov 12 wire reconnect.
  - Debouncing rejects most of the chatter, but dropouts of 10 ms or more get through as false pulses and false gusts.
  - Wind faults: fault 1 (Nov 12 20:06–22:10) is the disconnected wire; fault 2 (Nov 13 00:40–15:46) is the chatter.
- **Host side (unchanged by the port):**
  - `web_server.py`: dashboard and recorder, systemd user unit `anemometer-web`, port 8765.
  - `monitor.py`: payload decoding.
  - `maint.py`: maintenance windows.
  - `fault.py`: per-sensor faults.
  - `spindown.py`: bearing tests.

This is a closed record. Later lessons are in [lessons.md](lessons.md).
