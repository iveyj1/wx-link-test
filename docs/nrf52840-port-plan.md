# Port plan: sensor node on the nRF52840 Pro Micro

> **Parked 2026-11-16.** The bring-up code (board definition, probe, flash script) lives on branch
> `nrf52840-port`. `main` is back on the Feather M0. Findings below are kept for a later custom nRF52840 board.

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

### Vane power-up validation (bench, before relying on pulsed power)

The datasheet gives T_PU = 10 ms and a 2.2 ms slow-filter settling time. It doesn't say how soon after power-up the
analog OUT is accurate: the AGC, the DAC and any output glitch at power-on all matter. To measure:

1. **Fix the vane** at about 8 angles around the circle, including both sides of the 0/360° wrap.
2. **Reference:** with the AS5600 continuously powered, average many ADC readings at each angle.
3. **Power-cycle repeatedly** (e.g. 200 times per angle, with varied off-times). Sample OUT at 2, 5, 8, 10, 12, 15, 20,
   30 and 50 ms after switch-on, and also capture OUT on a scope during power-up.
4. **Pass criterion:** the earliest time at which every sample is within ±1° (≈ 11 counts) of the reference, at every
   angle.
   - The firmware wait is then twice that.
   - Check the I2C STATUS/AGC at the same moment, to see whether the AGC has settled.
5. **Repeat cold** (freezer, about −15 °C) and at low battery voltage.
6. **Firmware guard:** take two or three samples a millisecond or two apart at the end of the wait, accept only if they
   agree, and flag a settle failure in `power_bits`.

If pulsed readings fail, use LPM3 continuously (1.5 mA) or poll less often.

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
| P0.20 | **Charge inhibit** (5 V panel → charger switch; the MCU can only inhibit) | | P0.29 / AIN5 | **Vane: AS5600 OUT** (analog angle) |
| P0.22 | **Wind reed** | | P0.02 / AIN0 | **Battery NTC** (battery temperature) |
| P0.24 | **Rain reed** | | P1.15 | **I2C SDA**: SHT31 (0x44) + BMP180 (0x77) |
| P1.00 | **Vane 3.3 V switch** (load switch) | | P1.13 | **I2C SCL** |
| P0.11 | **Battery heater enable** (logic-level N-FET; heater powered from the panel only) | | P1.11 | **Panel present** (5 V through a 100 k/200 k divider) |
| P1.04 | **NTC excitation** (high only while reading, so the divider draws nothing between readings) | | P0.10 | Avoid: NFC pin (usable only after a UICR change) |
| P1.06 | Spare (charger STAT input or status LED) | | P0.09 | Avoid: NFC pin |

The board's own USB VBUS is detected inside the nRF52840 (POWER USBDETECTED), but the panel needs its own
"present" input (P1.11), because the charge switch can disconnect it from the charger.

### Pin reasoning

- **Grouping:** one side carries the radio, the reeds and the vane; the other carries power, analog and the local I2C bus.
  Each cable group lands on one edge.
- **No restricted pins:** the nRF52840 maps UARTE, TWIM and GPIO SENSE to any pin.
  - The assignment stays off the NFC pins (P0.09/P0.10) and the hard-to-solder bottom pads (P1.01, P1.02, P1.07).
  - It also avoids the internal pins: P0.13 (VCC switch on nice!nano-style boards), P0.15 (LED), P0.04 (battery sense
    on some boards) and P0.18 (reset).
- **Analog:** only the three exposed AIN pins (P0.02, P0.29, P0.31) can read analog: battery voltage, vane OUT and
  battery temperature. That uses them all.
  - Panel presence is therefore a digital input.
  - If the board turns out to sense the battery internally on P0.04, P0.31 is freed for a panel-voltage divider.
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

## Reference schematic vs. this board

`nice-nano-schamatic.png` is the nice!nano v2 schematic. The Teyleten clone follows its pinout (header, LED P0.15,
reset P0.18, VCC switch P0.13, crystals on P0.00/P0.01), but **its power section is different**, as the bring-up probe shows:

| | nice!nano schematic | Teyleten (measured 2026-11-15) |
|---|---|---|
| nRF supply | AP2112K-3.3 LDO → VDD and VDDH tied (normal mode) | **High-voltage mode** (MAINREGSTATUS = 1): VDDH = 4.25 V on USB, VDD = 3.29 V from the nRF's own REG0 |
| Battery sense | 806 k / 2 M divider to P0.04 | P0.04 floats: no divider. Read VDDH/5 internally instead |
| VCC pin | P-FET from the 3.3 V LDO (600 mA), gate pulled low by 2 MΩ, so **on by default; P0.13 high turns it off** | The board has one 3.3 V LDO (the other regulator-like part is the charger). VCC held up under **at least 120 mA** in a user test, so it comes from that LDO, not REG0 |
| Charger | LN2054 (TP4054-type), 10 k PROG ≈ 100 mA, no temperature input, orange LED on VBUS | **Fuman (FM) LTH7R/4054** (LCSC C2913899): 4054-type CC/CV linear charger, SOT-23-5 (CHRG, GND, BAT, VCC, PROG); current set by the PROG resistor (I ≈ 1000 V / R_PROG). **No temperature input** |
| Power path | VBUS through a Schottky diode, battery through a P-FET that turns off when VBUS is present | Not checked |
| Header (USB left, CCW from bottom left) | | B−, 006, 005?, GND, GND, 017, 020, 022, 024, 100, 011, 104, 106 / 009, 010, 111, 113, 115, 002, 029, 031, VCC, RST, GND, RAW, B+. Also P1.01, P1.02, P1.07 on separate pads. **Pin 3 reads "005" here but P0.08 on the nice!nano; confirm before using it for UART RX** |
| USB VBUS | Also wired to the nRF VBUS pin | Must be, because USB works. `USBREGSTATUS.VBUSDETECT` reads it with no GPIO |

Consequences:
- **VCC is good for the RYLR998** (≥ 120 mA tested; the radio at +2 dBm needs far less). Still to check: whether
  P0.13 switches it, and the LDO's quiescent current on battery (part of the sleep-current go/no-go).
- **The battery reading is free.** VDDH/5 on the SAADC, so P0.31 needs no divider.
- **No charging for now** (decision 2026-11-15). The board charger likely can't do a cold-charge window; revisit with an
  external charger with a TS/TEMP input.
- **VBUSDETECT reads the board's own VBUS.** If the charge switch sits between the panel and the board's USB, it reads
  the switched side. P1.11 is still needed for "panel present" upstream of the switch.

## Bring-up results, Nov 15–16 (Teyleten Pro Micro clone)

Measured with the probe sketch on branch `nrf52840-port` (`node52/`), no sensors attached.

| Check | Result |
|---|---|
| 32.768 kHz crystal | Fitted; −8.9 ppm. Internal RC: −0.24% uncalibrated |
| Reed input path (SENSE → PORT event → PPI → EGU3, 10 ms debounce on RTC2) | Correct for clean pulses, 0.3 ms bounce, 5 ms dropouts and 10 rev/s; a 15 ms dropout counts twice, as on the Feather |
| UART, one DMA transfer | 250 bytes in 21.72 ms, no gaps |
| Supply | High-voltage mode (VDDH); battery readable internally as VDDH/5; no divider on P0.04 |
| VCC pin | From the board's 3.3 V regulator, at least 120 mA. Enable is P0.13, **active high**, with a pull-up of about 5 kΩ: holding it low costs about 640 µA |
| Sleep current, System ON, RTC running, nothing attached | About 110 µA with VCC on (regulator marked J2RD, unidentified); about 750 µA with P0.13 held low |
| Charger | Fuman LTH7R/4054: no temperature input |
| Bootloader | Updated to Adafruit 0.11.0 (nice_nano build, S140 6.1.1). Software entry still fails: the bootloader clears GPREGRET, then starts the app within 0.2 s. Flashing needs a double-tap of RST |
| Not done | System OFF current (board-only floor); cause of the bootloader's early exit |

Conclusion: the nRF52840 itself suits the job (crystal, low-power edge detection, clean UART), but this clone's power
circuit does not. A production design would be a custom board: nRF52840 module, a regulator with about 1 µA quiescent
current, a load switch for the vane, and a charger with a temperature input.

## Power notes (to verify on the bench first)

- **Standby current:** the manual quotes "~1 mA" standby with the LED off. The nRF itself idles at a few µA, so the rest
  is the board's regulator, charger and LED.
  - **Go/no-go:** measure the bare board on battery in System ON sleep before wiring anything. The Feather's W8 deep
  standby is the baseline to beat.
- **VCC switch (measured 2026-11-15):** opposite to the nice!nano. On this board **P0.13 high = VCC on**, and
  VCC is on by default through a strong pull-up of roughly 5 kΩ: holding P0.13 low (VCC off) costs about 640 µA.
  Sleep with VCC on measured about 110 µA (System ON, RTC running, nothing attached); with P0.13 low, about 750 µA.
  So VCC stays on permanently; switch the vane (and any other load to be cut) with its own load switch.
  If it's switched, keep it on for the radio and BMP180 and use a separate load switch for the AS5600.
- **Battery sense:** this board has no P0.04 divider, but it runs in high-voltage mode, so VDDH/5 reads the battery
  internally. Confirm VDDH tracks the battery when running on battery alone.
- **Crystal:** confirm a 32.768 kHz crystal is fitted. If not, the LFRC with calibration is adequate: the host
  timestamps packets, and ±250 ppm is irrelevant to revolution periods.
- **Radio and UART current:** the firmware already sleeps the RYLR998 between sends (`AT+MODE=1`, 10 µA; RX 17.5 mA;
  TX 140 mA at +22 dBm, far less at our +2 dBm).
  - UARTE RX keeps the high-frequency clock running (hundreds of µA), so stop UARTE RX between radio transactions and
    restart it before the next command.
  - The radio's first command after sleep only wakes it, as now.

## Power switching, charging and battery temperature

A USB 5 V solar panel feeds two loads: the charger and, when needed, a battery heater. Three switches, all
default-off in hardware unless noted:

| Switch | Rail | Part type | Control |
|---|---|---|---|
| Vane | 3.3 V, ≈ 6.5 mA | Load switch with slew control (e.g. TPS22917), so the 3.3 V rail doesn't dip | P1.00, on for ≈ 13 ms per reading |
| Charge | Panel 5 V → charger | Load switch rated 5.5 V, or a P-FET with an N-FET level shifter | Hardware temperature window; P0.20 can only inhibit |
| Heater | Panel 5 V → heater | Logic-level N-FET, low side (e.g. AO3400) | P0.11, firmware thermostat; never runs from the battery |

### Cold-charge protection must not depend on the firmware

- **Two failure modes:**
  - If charging needs the MCU to turn it on, a flat battery can never recover: no battery, no MCU, no charging.
  - If the MCU alone is responsible for stopping cold charging, a firmware fault can charge a frozen cell.
- **Fix: enforce a 0–45 °C window in hardware,** and let the MCU only add inhibits on top (wired-AND).
  - The simplest way is an external charger with an NTC input fed from the panel. For example, a TP4056 module with its
    TEMP pin wired to a 10 k NTC network, or a BQ2407x/BQ25185-type charger with TS/JEITA. Its output goes to B+.
  - The board's own charger is then used only on the bench with USB.
- **Too hot as well:** the same window stops charging above 45 °C, which matters if the battery box sits in summer sun.

### Battery heater (charge on cold, sunny days)

- **Heater:** a resistor or polyimide film heater on the cell, about 1 W (25 Ω at 5 V), run only from the panel.
- **Insulation does most of the work:** a small EPS box (about 10 × 8 × 5 cm, 2 cm walls) loses roughly 0.06 W/K.
  - 1 W then holds the cell about 15 K above ambient.
  - A 60 g cell warms 10 K in roughly 10–15 minutes.
  - These are estimates. Measure the box's warm-up and cool-down on the bench.
- **Firmware thermostat:** while the panel is present and battery temperature is below about +3 °C, heat. Stop at about
  +8 °C. The hardware window lets charging start once the cell is above 0 °C.
- **Panel budget:** the heater must not starve charging. With a 5–10 W panel, 1 W leaves most of the output for the
  charger. On weak sun the panel voltage sags, which the panel-present input can catch.

**Passive solar heating** (a dark, insulated battery box on the sunny face, perhaps glazed) couples nicely: it's warm
exactly when the sun can charge. But it can't be switched off, so in summer the box must be shaded or vented, and the
45 °C cutoff becomes essential. The heater and passive heating can be combined.

### Battery temperature sensor

- **Sensor:** a 10 kΩ NTC (B ≈ 3950) bead taped to the cell, inside the insulation.
- **Divider:** with a 10 kΩ fixed resistor, read on P0.02/AIN0. The divider is powered from P1.04 only while reading,
  so it draws no idle current. The ADC is referenced to VDD, so the reading is ratiometric.
- **Second NTC:** the external charger's TEMP/TS network needs its own NTC on the same cell. Two beads keep the
  charger's protection independent of the MCU.

### USB detection

- VBUS from a panel or a charger isn't a USB host. Keep the Feather's rule: only an enumerated host (USB start-of-frame
  seen) switches to debug or "USB" behaviour.
- Panel presence comes from P1.11, VBUS from the nRF.

### Sleep current with all this fitted

The charger, the switches and the divider must not leak from the battery when the panel is dark. Include them all in
the sleep-current measurement before deployment.

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
- **Phase 2: W9** = W8 + `rh_dpct` (0.1 %RH), `sht_temp_dk`, `vane_raw` (ADC counts, 0–fff), `batt_temp_dk`, and
  `power_bits` (panel present, VBUS, charge inhibit, heater on, charger status, vane settle failure).
  - **Byte budget:** the W8 worst case is 223 of 240 bytes, and W9 adds ≈ 24, so it doesn't fit as is.
  - Options:
    - Cap the diagnostic counters (`raw_edges`, `rain_raw_edges`) at 32 bits, which saves up to 16 bytes.
    - Send the slow fields (humidity, the temperatures, battery, power bits) only every Nth packet and in heartbeats.
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
   STATUS/AGC). Then the **vane power-up validation** (see below).
6. **Payload W9 and host:** `monitor.py`, `web_server.py`, dashboard humidity, dew point and wind direction (rose and
   time series), vane north offset, health flags.
7. **Soak indoors** side by side with the Feather (drain, packet loss, counts), then outdoors, then retire the Feather.
8. **Power:**
   - USB charging with the node in low-power operation;
   - then the external charger with its hardware temperature window;
   - then the battery NTC and the heater, testing the box's warm-up in a freezer;
   - then the solar panel outdoors.
9. **Bluetooth:** configuration service, BTHome advertising, then BLE OTA DFU behind the physical maintenance gate.

## Decisions and open questions

- **Decided 2026-11-15:**
  - Vane on a 3-wire analog cable (1–2 ft).
  - Battery charging over USB, possibly from a 5 V USB solar panel.
  - Bluetooth eventually, for configuration, phone advertising and Android OTA DFU.
- **Open:**
  - Enclosure and connector choice.
  - Solar panel size.
  - Heater, passive solar heating, or both.
  - External charger part.
- **Added 2026-11-15:**
  - Power switches for the vane (3.3 V), charging (5 V) and a solar-powered battery heater.
  - A temperature sensor on the battery.
  - The vane's power-up reading must be validated.

## Reference datasheets (`docs/`)

| File | Part |
|---|---|
| `Teyleten-Robot-Pro-Micro-NRF52840.pdf` | Board manual (vendor page) |
| `NRF52840-DK.pdf` | Nordic nRF52840 DK documentation |
| `RYLR998.pdf` | Reyax RYLR998 LoRa module (AT commands, currents) |
| `AMS_AS5600_Datasheet_EN.pdf` | AS5600 magnetic angle sensor (vane) |
| `Datasheet_SHT3x_DIS.pdf` | Sensirion SHT3x humidity and temperature |
| `BMP180.pdf` | Bosch BMP180 barometer |
