# Port plan: sensor node on the nRF52840 Pro Micro

**Target:** Teyleten Robot "Pro Micro nRF52840". It is a SuperMini / nice!nano v2-style clone (nRF52840, 1 MB flash,
256 KB RAM, USB-C, LiPo charger, Adafruit UF2 bootloader; manual: `Teyleten-Robot-Pro-Micro-NRF52840.pdf`).

**Peripherals:**
- RYLR998 radio (unchanged)
- BMP180 barometer (unchanged)
- SHT31 temperature and humidity (new)
- AS5600 wind vane angle (new)
- Wind and rain reed switches

**Scope:**
- Only the sensor node moves. The receiver stays a Feather M0 running `link/`.
- The host stays as it is apart from decoding the new payload.
- The baseline is tagged `feather-m0-w8` (see [feather-m0-baseline.md](feather-m0-baseline.md)).

## Why it helps

| Feather M0 pain point | nRF52840 |
|---|---|
| RYLR998 splits commands on UART idle gaps, so TX needed hand-written DMA | UARTE always transmits from RAM by EasyDMA, so there are no gaps by construction |
| Deep standby needed GCLK/RTC/EIC routing, SysTick masking and RTC sync-hazard workarounds | System ON sleep (WFE) keeps the RTC, GPIO SENSE and the WDT running with no clock re-routing; idle is a few µA for the chip |
| Crystal failure needed a hand-built failover | The LFCLK source is selectable (LFXO, or LFRC calibrated to about ±250 ppm). The WDT forces the LFRC on by itself, so it survives a dead crystal |
| One I2C bus | Two TWIM instances, so the outdoor vane cable gets its own bus |
| 256 KB flash, 32 KB RAM | 1 MB flash, 256 KB RAM; BLE is available for future local maintenance |

## AS5600 (wind vane)

- **Output:** the angle is readable over I2C at fixed address **0x36**:
  - RAW ANGLE at 0x0C–0x0D (12-bit, 0–4095);
  - ANGLE, scaled by ZPOS/MPOS, at 0x0E–0x0F;
  - STATUS at 0x0B: MD = magnet detected, ML = too weak, MH = too strong;
  - AGC at 0x1A and MAGNITUDE at 0x1B–0x1C, for health and magnet-gap trends.
- **Calibration:** use RAW ANGLE and apply the north offset on the host, so it can be recalibrated without
  burning the chip's one-time-programmable memory.
- **Alternative output:** the OUT pin gives an analog (default, ratiometric 0–VDD) or PWM angle. An analog OUT line into
  the ADC needs only 3 wires, which is more robust over a long cable than I2C. I've left a pin free for it.
- **Power:** about 6.5 mA in normal mode (1.5 mA in its lowest low-power mode), the largest load after the radio. Plan:
  - power it through a load switch;
  - wake it, wait for the first valid sample, read the angle and STATUS, then cut power, once per report (≤ 1/s);
  - that averages ≈ 0.1 mA at 1 report/s, or far less at the 60 s idle heartbeat;
  - its I2C pull-ups go on the switched rail, so nothing back-powers it when off.
  - Measure the power-up-to-valid time on the bench (datasheet: a few ms).
- **Wiring:** tie DIR to GND (or VDD) to set which way the angle increases. For 3.3 V operation, VDD5V and VDD3V3 are tied
  together (most breakouts do this with a jumper or solder bridge).

## Proposed pin assignments

Orientation: component side up, USB at the top. The silkscreen numbers are the nRF port.pin (e.g. `022` = P0.22,
`104` = P1.04).

| Left side | Use | | Right side | Use |
|---|---|---|---|---|
| P0.06 | UART TX → RYLR998 RXD | | B+ (RAW) | LiPo + |
| P0.08 | UART RX ← RYLR998 TXD | | GND | LiPo − |
| GND | Radio GND | | RST | Reset; double-tap to GND for the UF2 bootloader (bring out to a pad or button) |
| GND | Sensor GND bus | | VCC | 3.3 V to peripherals (see the power notes) |
| P0.17 | RYLR998 NRST (open-drain, active low) | | P0.31 / AIN7 | Battery divider, if the board has no battery sense |
| P0.20 | Spare (future radio load switch) | | P0.29 / AIN5 | Optional AS5600 OUT (analog angle) |
| P0.22 | **Wind reed** | | P0.02 / AIN0 | Spare analog (e.g. solar) |
| P0.24 | **Rain reed** | | P1.15 | **I2C0 SDA**: SHT31 (0x44) + BMP180 (0x77) |
| P1.00 | AS5600 power enable (load switch) | | P1.13 | **I2C0 SCL** |
| P0.11 | **I2C1 SDA**: AS5600 (0x36) | | P1.11 | Spare (e.g. SHT31 ALERT) |
| P1.04 | **I2C1 SCL** | | P0.10 | Avoid: NFC pin (usable only after a UICR change) |
| P1.06 | Spare (status LED, external) | | P0.09 | Avoid: NFC pin |

### Pin reasoning

- **Grouping:** one side carries the radio, the reeds and the vane; the other carries power, analog and the local I2C bus.
  Each cable group lands on one edge.
- **No restricted pins:** the nRF52840 maps UARTE, TWIM and GPIO SENSE to any pin.
  - The assignment stays off the NFC pins (P0.09/P0.10) and the hard-to-solder bottom pads (P1.01, P1.02, P1.07).
  - It also avoids the internal pins: P0.13 (VCC switch on nice!nano-style boards), P0.15 (LED), P0.04 (battery sense
    on some boards) and P0.18 (reset).
- **Analog:** only the three exposed AIN pins (P0.02, P0.29, P0.31) can read analog; all three are kept for battery,
  vane OUT and a spare.
- **Two I2C buses:** the vane's cable runs outdoors. A short, damp or unpowered AS5600 can hang its own bus but not the
  barometer and humidity bus.
- **Addresses:** 0x44, 0x77 and 0x36 don't clash.

## Robustness changes (lessons from the Feather)

1. **Reed inputs** (the Nov 13 chatter came from an intermittent joint, with dropouts of 10 ms or more):
   - **Pull-up:** external 47–100 kΩ to 3.3 V instead of the internal ≈ 13 kΩ. The internal one would draw 250 µA
     whenever the anemometer parks with the switch closed.
   - **Input filter:** 1 kΩ in series plus 10 nF to GND at the board.
   - **ESD:** a TVS or clamp diode on each outdoor line.
   - **Firmware:** keep the 10 ms debounce and the raw edge counters.
2. **Connectors:** locking connectors (JST-GH/PH, or screw terminals) with strain relief on every outdoor cable, and no
   bare splices.
3. **Fault flags in the payload:**
   - I2C errors per bus, BMP180/SHT31 CRC or ID failures, and the AS5600 STATUS bits;
   - a chatter indicator: raw edges per debounced pulse over the last report.
   - The host can use these to open or close `fault.py` faults automatically.
4. **I2C bus recovery:** 9 SCL clocks plus a STOP after any timeout, and a per-bus error counter.
5. **SHT31:** single-shot measurements, high repeatability, with the CRC checked. Use the heater only for a periodic
   condensation check, flagged in the payload so heated readings aren't used.
6. **Conformal coat** the board as before. Keep the SHT31 sensing window and the BMP180 port clear.

## Power notes (to verify on the bench first)

- **Standby current:** the manual quotes "~1 mA" standby with the LED off. The nRF itself idles at a few µA, so the rest
  is the board's regulator, charger and LED.
  - **Go/no-go:** measure the bare board on battery in System ON sleep before wiring anything. The Feather's W8 deep
  standby is the baseline to beat.
- **VCC switch:** on nice!nano-style boards the VCC pin is switched by P0.13. Check this board's behaviour with a meter.
  If it's switched, keep it on for the radio and BMP180 and use a separate load switch for the AS5600.
- **Battery sense:** some clones divide the battery onto P0.04 internally; others have nothing. If this one has none,
  use 2 × 1 MΩ to P0.31 with 100 nF across the lower resistor.
- **Crystal:** confirm a 32.768 kHz crystal is fitted. If not, the LFRC with calibration is adequate: the host
  timestamps packets, and ±250 ppm is irrelevant to revolution periods.
- **UART current:** UARTE RX keeps the high-frequency clock running (hundreds of µA). That's negligible against the
  RYLR998 in receive mode (≈ 17 mA). It would matter only if the radio is later put to sleep between reports.

## Firmware structure

```
common/   portable, host-tested: PulseCounter.h, TipCounter.h, ReportSchedule.h, Bmp180.h,
          Payload.h (moved out of link.ino), Sht31.h (new, CRC), As5600.h (new)
link/     Feather M0 sensor + receiver, unchanged except including common/
node52/   nRF52840 sensor: HAL for RTC2, GPIO SENSE, UARTE, TWIM0/1, SAADC, WDT, power
```

- **Toolchain:** Adafruit nRF52 Arduino core (matches the UF2 bootloader with S140 6.1.1), plus a SuperMini/nice!nano
  board definition (e.g. the "nRFMicro"/SuperMini package the manual mentions). Install it with arduino-cli. The
  `arduino:nrf52` core already installed here is the old Primo core and isn't suitable. Shared code goes in with
  `arduino-cli compile --library common`, and `build.sh` gains a `node52` target.
- **Timebase:** RTC2 with PRESCALER 0 (32768 ticks/s, the same unit as today). Its 24-bit counter is extended to 64 bits
  on overflow. RTC0 and RTC1 stay free for the SoftDevice and FreeRTOS.
- **Reed edges:** PORT/SENSE interrupts, not GPIOTE IN channels, which cost noticeably more current. They're debounced
  by an RTC2 compare and fed to the unchanged PulseCounter/TipCounter.
- **BLE:** off by default; it's an option later for configuration and diagnostics.

## Payload

- **Phase 1:** the node sends W8 unchanged (sleep = 1, clock = LFRC flag), so it can be run side by side with the Feather
  on the existing host.
- **Phase 2: W9** = W8 + `rh_dpct` (0.1 %RH), `sht_temp_dk`, `vane_raw` (0–fff), `vane_status`, `fault_bits`.
  - **Byte budget:** the W8 worst case is 223 of 240 bytes, and W9 adds ≈ 20, so it's tight.
  - Cap `raw_edges`/`rain_raw_edges`, which are diagnostic, at 32 bits to restore margin.
  - The receiver's 255-character line limit also needs checking.
- **Wind direction:** the host computes the vector mean and the direction at the gust from per-report vane samples. Fixed
  vector math on the sensor isn't needed.

## Phases

1. **Bench bring-up:** board package, blink, USB serial, bootloader version (`INFO_UF2.TXT`). Then **bare-board sleep
   current**, the VCC switch, the battery sense and the crystal check. This is the go/no-go.
2. **Timebase and watchdog:** RTC2, the 64-bit tick extension, the WDT, LFXO start with LFRC fallback.
3. **Reed inputs:** SENSE + debounce driving the shared counters; compare against a signal generator or a hand spin.
4. **Radio:** UARTE, AT configuration, NRST recovery. Send W8 to the existing receiver and check against the Feather.
5. **Sensors:** BMP180 port, then SHT31 and AS5600 (power-gated, STATUS and AGC logged).
6. **Payload W9 and host:** `monitor.py`, `web_server.py`, dashboard humidity, dew point and wind direction (rose and
   time series), vane calibration offset, health flags.
7. **Soak indoors** side by side with the Feather (drain, packet loss, counts), then outdoors, then retire the Feather.

## Open questions

- **Vane cable:** length and type? It decides between I2C and the analog OUT line.
- **Power source:** battery only as now, or solar? That would use the spare AIN pin and change the charging setup.
- **Enclosure and connectors:** what will the new prototype use?
- **BLE:** wanted at all, for example for configuration without opening the enclosure?
