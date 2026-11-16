# Baseline: Feather M0 sensor (git tag `feather-m0-w8`)

This is a record of the working system as of 2026-11-15, before the move to the nRF52840 Pro Micro
(see [nrf52840-port-plan.md](nrf52840-port-plan.md)). To rebuild it: `git checkout feather-m0-w8 && ./build.sh`.

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

## Rebuild (from 2026-11-16) and lessons learned

The nRF52840 port is parked on branch `nrf52840-port` (the Pro Micro clone was hard to work with: a pulled-up VCC
enable that costs ~640 µA to hold off, ~110 µA sleep floor with nothing attached, and a bootloader that ignores
software DFU requests). Production may return to the nRF52840 on a custom board. Meanwhile the sensor is being
rebuilt on a **new Feather M0** with the parts in hand, in a new enclosure. Fast (≤ 1 s) reports stay for development.

Lessons to carry into the rebuild:
- **Power (from the logged battery voltage, 1200 mAh LiPo):** calm with one report a minute ≈ 0.14 mV/h (≈ 0.2 mA);
  windy with ~1 report a second ≈ 1.7 mV/h (≈ 2–3 mA); overall ≈ 4–5 weeks per charge. The send rate, not the sleep
  floor, dominates. For production: send every ~10 s and carry the peak gust and shortest revolution in the payload.
  Still to measure with a meter: the calm floor (battery lead) and the RYLR998 in `AT+MODE=1` (its VCC lead, ~10 µA expected).
- **Reed wiring:** the Nov 12–13 chatter was the connections. Use soldered or crimped, strain-relieved joints and a
  proper connector at the enclosure wall. Consider an RC filter at each reed input (e.g. 1 k series + 100 nF to GND
  at the pin, ~0.1 ms) for ESD and noise; firmware debounce cannot reject dropouts ≥ 10 ms.
- **Moisture:** conformal-coat the new board, especially the 32 kHz crystal and its caps (the old one failed over
  overnight before coating).
- **Bearings:** the 608CE pair is rough after reassembly; a new housing is planned. Re-run a spin-down after rebuild.
- **New board identity:** the new Feather has a new USB serial number; update the sensor ID in the hardware notes.
  The receiver (`monitor.py` RECEIVER) is unchanged.
- **Before deploying:** bench soak with the raw edge counters at zero while wiggling the cables; measure the floor;
  then close maintenance window 6 when it goes back outside.
