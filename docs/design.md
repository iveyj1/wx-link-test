# Measurement design

How each quantity is measured, reported and displayed. Pinout, packet fields and timing tables are on the dashboard's
reference page (`/reference.html`, source `web/reference.html`).

### Wind
* **Timebase:** RTC at 32 768 Hz from the crystal, extended to 64 bits in software, so intervals have no wrap limit.
* **Debounce:** each D5 change restarts a 328-tick (10.0 ms) one-shot; a level must hold that long to count. The timestamp is the last edge before the stable interval.
* **Revolutions:** the first closure after boot is the reference; closures 3, 5, 7… complete revolutions. A and B are the two half-intervals; period = A + B. No equal-spacing assumption; A/B labels can swap after a restart. A closure held at boot is not counted.
* **Speed:** Hz = 1/period of the latest *completed* revolution; RPM = 60 × Hz; knots = max(0, gain × Hz + offset), with the calibration entered per browser (currently 6.5 kn per rev/s; field points are on the reference page).

### Rain
* Each debounced D10 closure is one tip; the reopening is ignored. The default calibration is **0.3690 mm/tip**: 2 ml per tip over a 49.5 × 109.5 mm funnel (2000 mm³ ÷ 5420.25 mm²). Check it by pouring a known volume (100 ml ≈ 50 tips ≈ 18.4 mm).
* The dashboard shows the total from local midnight to midnight, hourly bars, and rate = mm/tip ÷ max(last tip interval, time since last tip). That rate decays during dry spells and shows 0 after 1 h without a tip. Last tip and rate use only counted tips (outside maintenance windows and gauge faults, `rain_last` in the status); restarts and gaps inside a window do not qualify a rain total; the battery slope uses only readings since the last window ended.
* The chart shows 1–4 days; each day's line restarts from 0 at local midnight, with the day's total above it.
* **Total since a mark** (per browser): **Mark now** (like emptying a manual gauge), a date and time, or a preset (last 1 h/24 h/72 h/7 days, month, year, water year from Oct 1). The card warns when the total is incomplete: tips not yet reported before a **sensor restart** are lost; **data gaps** over 1 h are harmless within one sensor run, because counts are cumulative; and nothing is counted before rain logging began.

### Reporting and packets
* A completed revolution or a rain tip triggers a report immediately, limited to one send per second (from completion), with a 5 s backoff after a failure and a 60 s heartbeat otherwise. Events during the cooldown are coalesced.
* **Counts are cumulative since boot**, so a lost packet loses no pulses or tips; the next packet carries the totals. Counts are RAM-only and restart with a new session ID on reset.
* **W9 payload** = W8 + `,air_dk,rh_pm` (SHT31 air temperature in 0.1 K and relative humidity in 0.1 %, both 0 when there is no fresh reading; worst case 231 bytes). The field-by-field table is on the dashboard's reference page (`/reference.html`).
* **W8 payload** = W7 + `,sleep` (0 = light sleep, 1 = deep standby; worst case 223 bytes). **W7 payload** (all numbers lowercase hex, worst case 221 bytes, under the radio's 240-byte limit and the receiver's 255-character line): `W7,session,seq,count,A,B,age,rev_age,uptime,pin,pin_age,raw_edges,clock,rain_tips,rain_interval,rain_age,rain_pin,rain_raw_edges,vbat_mv,pressure_pa,temp_dk`. A, B and `rain_interval` are capped at `ffffffff` (36 h) to make room; `temp_dk` is 0.1 K (°C × 10 + 2732); both barometer fields are 0 when there is no reading. W6 is the same without the barometer fields and uncapped (worst case 235); W5 also lacks `vbat_mv`. Revolutions and period are derived by the decoder. Ages of `ffffffffffffffff` mean "never". `pin` = 1 is open, `clock` = 1 is the internal fallback. The receiver forwards `+RCV=1,len,payload,RSSI,SNR`. The decoder still accepts W1–W8.

### Battery
* `vbat_mv` is read from A7/D9 (the Feather's 1:2 battery divider): eight 12-bit reads averaged, 3.3 V full scale, sampled when each report is built. On USB it shows the charger (about 4.2 V).
* The dashboard's Battery panel shows the latest voltage, the last-6-h least-squares trend in mV/h with a rough time to 3.5 V, and a 6 h–7 day chart (mean line and min–max band). `/api/battery?start=&end=` returns per-bucket mean/min/max from the packet log (7 days).
* **Measured with W7 (light sleep only):** 4.20 → 3.82 V in 46 h outdoors (Nov 7–8), about 5–9 days per charge, matching the estimate of 5–8 mA dominated by the MCU at 48 MHz in light sleep (~4–6 mA). Radio ≈ 0.8 mA at one report per 3 s. **W8 deep standby** is expected to bring this to roughly 1 mA (6–7 weeks); compare the Battery panel's mV/h trend before and after.

### Barometer (BMP180)
* Probed at the first report after boot (chip ID 0x55, calibration EEPROM checked), read at most every 30 s inside a report at ultra-high resolution (oss 3, about 31 ms of `delay()` while counting continues in interrupts). Readings older than 5 min are sent as 0. If it is absent or keeps failing, it is probed again every minute, so it can be connected later.
* A reset during an I2C access disables the I2C sensors until the next reset, using the breadcrumb (`i2c` stage), so a bad I2C bus cannot cause a reset loop. (Planned change F6: replace this with a 1-minute re-check.)
* Pressure is recorded as raw **station pressure**. The dashboard adds an optional correction and reduces to sea level with the altimeter-setting formula, so it compares directly with METAR altimeter settings. These are already reduced to sea level, so only this station's elevation matters (938 ft = 285.9 m), not the airport's. Check, Nov 6 23:34: KARB 30.03 inHg implies 982.93 hPa here, and the sensor read 982.93 hPa, so the default correction is 0. Settings has a "Calibrate to it now" helper; repeat on a few days (METARs are rounded to 0.34 hPa), especially once the sensor is outside and colder.
* The dashboard shows pressure, its 3 h tendency, temperature, and 6 h–30 day charts. `!i2c-scan` lists I2C devices; `!status` includes `baro` and `air` lines.

### Air temperature and humidity (SHT31)
* On the same bus as the BMP180, at 0x44 (or 0x45). Single-shot, high-repeatability measurements at most every 30 s inside a report; both words are CRC-8 checked (`link/Sht31.h`, tested natively). Readings older than 5 min are sent as 0; an absent sensor is probed again every minute.
* The measurement is polled until the sensor answers. A genuine SHT31 needs 15.5 ms at most; the first chip fitted (Nov 16) acknowledged commands but never delivered a measurement and was replaced.
* Stored per packet (`air_temp_c`, `rh_pct`) and one row a minute in `climate` (400 days). Fault sensor name: `air`.
* It reads air temperature only when mounted outside the enclosure in a radiation shield. The BMP180's temperature is the enclosure's.

### I2C bus (software driver)
* The sensors are driven by a bit-banged I2C master (`link/SoftI2c.h`, tested natively against a simulated device by `test_softi2c.cpp`), about 50 kHz, open drain through the internal pull-ups plus the breakouts' own. Every wait has a timeout (20 ms of clock stretching), and each sensor access starts with a bus clear (9 clocks and a STOP).
* Reason: the SAMD21 `Wire` driver has no timeouts and locked the board up in `setup()` (Nov 16).
* The watchdog starts before the first I2C access. SDA and SCL are checked at boot; if either is low, I2C stays off and is re-checked every minute.
* `!i2c-probe` reports the idle line levels with and without the internal pull-ups, which addresses acknowledge, and a CRC-checked SHT31 status read. `!sht` re-runs the SHT31 setup and one measurement and prints each step.

### Dashboard display rules
* Gaps are not zero wind: lines break only on missing revolutions, sensor restarts or no data.
* **Deduced calm (display only):** a half-revolution longer than 20 s means < 1.5 RPM, below resolution. The page draws 0 (hollow dots, dashed line) from 20 s after that half began until the closure that ended it, instead of the spanning revolution's meaningless average. A provisional zero is drawn live. A dashed zero during obvious wind points to a missed closure (wiring), not calm.
* **Calm on the live cards:** the same argument applies to the current reading. Speed and RPM show 0, and the period shows "> N s", once no closure for 20 s is *certain*. The sensor reports a middle closure only with the next heartbeat, so after a completed revolution that takes 40 s; after a known middle closure, 20 s (plus 3 s latency margin). The status then reads "Calm · no closure for …". A stale radio link is never treated as calm.
* **Link and sensor card:** RSSI and SNR (latest, 1 h average and minimum), last-packet age and sequence, missed packets over 1 h and 24 h (sequence numbers skipped within each sensor session), battery (W6), 32 kHz clock source, both reed states, protocol, session and uptime, malformed frames. Link statistics include maintenance time and are cached for 20 s.
* During maintenance the cards keep showing live readings; only graphs, averages and rain totals leave the data out.
* Each strip's right column shows max, time-weighted average (revolutions ÷ time, calm = 0), min, n and calm time; min/max come from real points.
* Strips can be 10 s to 24 h long, aligned to local time (24 h strips start at local midnight; a strip spanning a DST change is offset by an hour). Strips over 1 h load **condensed** history: per bucket (strip ÷ 720), the slowest and fastest revolution, so extremes and calms survive, plus revolution and calm totals for an exact strip average. Dots are drawn only while they fit (about one per 4 px).
* The balance between half-turns (A vs B) is not shown: reed chatter makes it too erratic to use until the hardware is improved. A and B are still listed, because a very short one points to chatter.

## Spin-down tests (bearings)

Spin the cups by hand indoors (inside a maintenance window), let them stop completely, wait ~20 s, repeat about 5 times per bearing, then run `python3 spindown.py --since HH:MM [--until HH:MM] --label "608ZZ stock" [--csv points.csv]`. It splits the spins, and fits `df/dt = -a - b·f²` from the exact revolution count (`d(f²)/dn` is linear in `f²`), using about one speed point per second (the report rate). `a` (rev/s²) is the constant friction, mostly the bearing; `b` (1/rev) is the drag on the cups. Compare `a`, `b` and the fixed-band metrics (coast time 2 → 0.5 rev/s, revolutions from 2 rev/s to the stop), not total revolutions: hand spins start at different speeds. The dashboard's **Spin-down tests** page (`/spindown.html`, data from `/api/spindown`) presents every set listed in `spindown_sets.json` (label, start, end, note, `reference`): per-set results, per-spin strips for a, b and coast time, speed curves with the fitted model, the linearised fit, and the model write-up. Add a set by appending its time range to that file; the page picks it up on reload. No firmware change is needed. `test_spindown.py` checks the fit on simulated spins sampled like the sensor's reports.
