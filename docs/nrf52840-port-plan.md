# Port plan: sensor node on the nRF52840 Pro Micro

**Target:** Teyleten Robot "Pro Micro nRF52840". It is a SuperMini / nice!nano v2-style clone (nRF52840, 1 MB flash,
256 KB RAM, USB-C, LiPo charger, Adafruit UF2 bootloader; manual: `Teyleten-Robot-Pro-Micro-NRF52840.pdf`).

**Peripherals:**
- RYLR998 radio (unchanged)
- BMP180 barometer (unchanged)
- SHT31 temperature and humidity (new)
- AS5600 wind vane angle (new, 3-wire analog)
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
| No humidity sensor or wind vane | Spare I2C and analog inputs for the SHT31 and the AS5600 vane; a second TWIM if needed |
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
- **Chosen: 3-wire analog** (decided 2026-11-15; cable about 1–2 ft). The wires are switched VDD, GND and OUT, with OUT
  read by the SAADC.
  - **Settings:** the default output (OUTS = 00) is analog, ratiometric 0–100% of VDD over 0–360°. No I2C writes or OTP
    burn are needed.
  - **ADC:** read with the VDD-referenced input (reference VDD/4, gain 1/4), so supply variation cancels.
  - **Diagnostics:** keep the AS5600's SDA/SCL reachable on the breakout. The I2C STATUS and AGC registers help when
    setting the magnet gap on the bench, but aren't wired in service.
- **Power** (datasheet: 6.5 mA normal mode, 1.5 mA in LPM3; power-up T_PU 10 ms; settling 2.2 ms at SF = 00):
  - power it through a load switch, once per report (≤ 1/s): on, wait ≈ 13 ms, sample OUT, off;
  - that's ≈ 85 µC per reading, so ≈ 85 µA at 1 report/s and ≈ 1.4 µA at the 60 s idle heartbeat.
  - LPM3 always on would cost 1.5 mA, far more.
- **Lost by not using I2C:** the STATUS bits (magnet detected, too weak, too strong). The analog line can't report a
  missing magnet, so the host should flag a vane stuck at one value or reading at the rail, or one that doesn't track
  wind changes.
- **Wiring:**
  - Tie DIR to GND (or VDD) to set which way the angle increases.
  - For 3.3 V operation, VDD5V and VDD3V3 are tied together (most breakouts do this with a jumper or solder bridge).
  - Add 1 kΩ in series and 100 nF to GND at the ADC pin (τ = 0.1 ms) to filter pickup on the cable.

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
| P0.20 | Cold-charge inhibit (USB/solar input switch), if fitted | | P0.29 / AIN5 | **Vane: AS5600 OUT** (analog angle) |
| P0.22 | **Wind reed** | | P0.02 / AIN0 | Spare analog (e.g. solar panel voltage) |
| P0.24 | **Rain reed** | | P1.15 | **I2C SDA**: SHT31 (0x44) + BMP180 (0x77) |
| P1.00 | **Vane power enable** (load switch) | | P1.13 | **I2C SCL** |
| P0.11 | Spare | | P1.11 | Spare (e.g. SHT31 ALERT) |
| P1.04 | Spare | | P0.10 | Avoid: NFC pin (usable only after a UICR change) |
| P1.06 | Spare (status LED, external) | | P0.09 | Avoid: NFC pin |

USB VBUS (USB or solar present) is detected inside the nRF52840 (POWER USBDETECTED/USBREMOVED), so it needs no pin.

### Pin reasoning

- **Grouping:** one side carries the radio, the reeds and the vane; the other carries power, analog and the local I2C bus.
  Each cable group lands on one edge.
- **No restricted pins:** the nRF52840 maps UARTE, TWIM and GPIO SENSE to any pin.
  - The assignment stays off the NFC pins (P0.09/P0.10) and the hard-to-solder bottom pads (P1.01, P1.02, P1.07).
  - It also avoids the internal pins: P0.13 (VCC switch on nice!nano-style boards), P0.15 (LED), P0.04 (battery sense
    on some boards) and P0.18 (reset).
- **Analog:** only the three exposed AIN pins (P0.02, P0.29, P0.31) can read analog; all three are kept for battery,
  vane OUT and a spare.
- **One I2C bus:** with the vane on an analog line, only the SHT31 and BMP180 (0x44 and 0x77, no clash) share the bus.
  The vane cable carries no I2C, so a fault on it can't hang the bus. A second TWIM stays available on the spare pins if
  ever needed.

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
- **Radio and UART current:** the firmware already sleeps the RYLR998 between sends (`AT+MODE=1`, 10 µA; RX 17.5 mA;
  TX 140 mA at +22 dBm, far less at our +2 dBm).
  - UARTE RX keeps the high-frequency clock running (hundreds of µA), so stop UARTE RX between radio transactions and
    restart it before the next command.
  - The radio's first command after sleep only wakes it, as now.

## Charging, solar and USB

- **Charging:** the board charges the LiPo from USB VBUS (5 V). A USB 5 V-output solar panel can feed the USB-C port.
- **Cold charging:** a LiPo must not be charged below 0 °C, and this board's charger has no temperature input. In
  winter the panel will try to charge a frozen cell. Options:
  1. Put a load switch (P-FET) in the panel's 5 V line, driven from P0.20. The firmware opens it below about +2 °C,
     using the SHT31 or BMP180 temperature, and closes it above about +5 °C. This is the minimal change.
  2. Use an external solar charger with an NTC input (and ideally MPPT) instead of the on-board one.
- **Sleep current with a charger fitted:** the charger chip's drain from the battery when VBUS is absent should be
  small, but include it in the bare-board sleep measurement.
- **USB detection:** VBUS from a solar panel isn't a USB host. Keep the Feather's rule:
  - only an enumerated host (USB start-of-frame seen) switches to debug or "USB" behaviour;
  - VBUS alone only means charging is possible, and is reported as a flag.
- **Payload flags:** add `vbus` and `charge_inhibit`, so the dashboard can show charging state alongside battery
  voltage.

## Bluetooth (later phase)

The Adafruit nRF52 core runs the S140 SoftDevice and includes the Bluefruit library, which covers all three goals.

- **Configuration:** the BLE UART service (or a small custom GATT service) feeds the same `!status`/command parser as
  USB serial: north offset, report interval, calibration gain and offset, cold-charge thresholds. Settings are stored in
  flash (LittleFS / InternalFileSystem).
- **Advertising to a phone:** put the current readings in the advertising data. **BTHome v2** is a published format that
  Home Assistant and several phone apps decode without a custom app; nRF Connect shows raw manufacturer data for
  debugging.
  - Advertising every 1–2 s at 0 dBm costs roughly 10–20 µA on average. Check it in the power budget.
- **OTA DFU from Android:** the Adafruit bootloader supports BLE DFU with Nordic's "nRF Device Firmware Update" / nRF
  Connect app, and the sketch exposes `BLEDfu` to reboot into it.
  - **Verify** that this board's bootloader build includes BLE DFU (`INFO_UF2.TXT` and a test update) before relying on it.
  - **Security:** Adafruit's DFU is unsigned, so anyone in range could reflash the node. Expose `BLEDfu` and configuration
    writes only in a maintenance window opened physically (e.g. holding a magnet on the rain reed at boot, or the first
    few minutes after a reset), and/or require a bonded phone.
- **Radio coexistence:** BLE (2.4 GHz) and LoRa (915 MHz) don't interfere, but keep the board's chip antenna clear of
  the RYLR998 and of metal in the enclosure.

## Firmware structure

```
common/   portable, host-tested: PulseCounter.h, TipCounter.h, ReportSchedule.h, Bmp180.h,
          Payload.h (moved out of link.ino), Sht31.h (new, CRC), Vane.h (new: angle from ADC counts)
link/     Feather M0 sensor + receiver, unchanged except including common/
node52/   nRF52840 sensor: HAL for RTC2, GPIO SENSE, UARTE, TWIM0, SAADC, WDT, power, later BLE
```

- **Toolchain:** Adafruit nRF52 Arduino core (matches the UF2 bootloader with S140 6.1.1), plus a SuperMini/nice!nano
  board definition (e.g. the "nRFMicro"/SuperMini package the manual mentions). Install it with arduino-cli. The
  `arduino:nrf52` core already installed here is the old Primo core and isn't suitable. Shared code goes in with
  `arduino-cli compile --library common`, and `build.sh` gains a `node52` target.
- **Timebase:** RTC2 with PRESCALER 0 (32768 ticks/s, the same unit as today). Its 24-bit counter is extended to 64 bits
  on overflow. RTC0 and RTC1 stay free for the SoftDevice and FreeRTOS.
- **Reed edges:** PORT/SENSE interrupts, not GPIOTE IN channels, which cost noticeably more current. They're debounced
  by an RTC2 compare and fed to the unchanged PulseCounter/TipCounter.
- **BLE:** off until the Bluetooth phase (see above).

## Payload

- **Phase 1:** the node sends W8 unchanged (sleep = 1, clock = LFRC flag), so it can be run side by side with the Feather
  on the existing host.
- **Phase 2: W9** = W8 + `rh_dpct` (0.1 %RH), `sht_temp_dk`, `vane_raw` (ADC counts, 0–fff), `fault_bits` (including
  `vbus` and `charge_inhibit`).
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
5. **Sensors:** BMP180 port, then SHT31, then the vane (power-gated analog; set the magnet gap on the bench using I2C
   STATUS/AGC).
6. **Payload W9 and host:** `monitor.py`, `web_server.py`, dashboard humidity, dew point and wind direction (rose and
   time series), vane north offset, health flags.
7. **Soak indoors** side by side with the Feather (drain, packet loss, counts), then outdoors, then retire the Feather.
8. **Charging:** USB charging with the node in low-power operation, then the solar panel and the cold-charge inhibit.
9. **Bluetooth:** configuration service, BTHome advertising, then BLE OTA DFU behind the physical maintenance gate.

## Decisions and open questions

- **Decided 2026-11-15:**
  - Vane on a 3-wire analog cable (1–2 ft).
  - Battery charging over USB, possibly from a 5 V USB solar panel.
  - Bluetooth eventually, for configuration, phone advertising and Android OTA DFU.
- **Open:**
  - Enclosure and connector choice.
  - Solar panel size and the cold-charge approach (load switch versus an external charger).

## Reference datasheets (`docs/`)

| File | Part |
|---|---|
| `Teyleten-Robot-Pro-Micro-NRF52840.pdf` | Board manual (vendor page) |
| `NRF52840-DK.pdf` | Nordic nRF52840 DK documentation |
| `RYLR998.pdf` | Reyax RYLR998 LoRa module (AT commands, currents) |
| `AMS_AS5600_Datasheet_EN.pdf` | AS5600 magnetic angle sensor (vane) |
| `Datasheet_SHT3x_DIS.pdf` | Sensirion SHT3x humidity and temperature |
| `BMP180.pdf` | Bosch BMP180 barometer |
