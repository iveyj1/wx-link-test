# wx-link-test: Feather M0 / RYLR998 wind and rain link

A battery sensor counts anemometer and tipping-bucket rain-gauge reed closures, reads a BMP180 barometer, and reports over LoRa. A USB-attached receiver forwards packets to a host, which records them in SQLite and serves a live LAN dashboard.

The sensor runs firmware **W7-baro1** (battery voltage, plus BMP180 pressure and temperature; flashed 2026-11-06). The receiver runs the same sketch built with `SENSOR=0`.

## Hardware

| Role | Feather M0 USB serial ID | LoRa address |
|---|---|---|
| Sensor | `8FFE7CD9514D4D5447202020FF01092A` | 1 |
| Receiver | `B55AB548514D4D5447202020FF012F18` | 2 |

Use `/dev/serial/by-id/usb-Adafruit_Feather_M0_<ID>-if00` for monitoring; `ttyACMn` numbers change.

Sensor wiring (all inputs use the internal pull-up, active low):

| Pin | Connection |
|---|---|
| D5 (PA15, EXTINT15) | Anemometer reed to GND: two closures per revolution |
| D10 (PA18, EXTINT2) | Rain-gauge reed to GND: one ~100 ms closure per tip |
| D6 | RYLR998 NRST (active low); harmless if unwired |
| D0 / D1 | RYLR998 TXD / RXD (radio UART, 115200 baud) |
| SDA / SCL (PA22 / PA23) | BMP180 barometer, I2C address 0x77, 100 kHz |

Radio: 915 MHz, SF7, BW 500 kHz, CR 4/5, preamble 12, network 18, 2 dBm (configured and read back at startup; not a regulatory assessment). Do not use D9: it is the Feather's battery-voltage divider.

## Layout

| Path | Purpose |
|---|---|
| `link/link.ino` | Firmware for both roles (`-DSENSOR=1/0`) |
| `link/PulseCounter.h` | Wind reed debounce and revolution pairing |
| `link/TipCounter.h` | Rain reed debounce and tip counting |
| `link/Bmp180.h` | BMP180 compensation (datasheet integer maths; tested natively) |
| `link/ReportSchedule.h` | Report triggering, 1 s cooldown, 5 s backoff, 60 s heartbeat |
| `link/RadioUart.h` | Bounded polled UART transmit (see *Lessons learned*) |
| `link/Diagnostics.h`, `retained.ld` | Execution-stage breadcrumb kept in `.noinit` RAM across resets |
| `monitor.py` | Packet decoder (W1–W5) and terminal monitor |
| `web_server.py` | Serial reader, SQLite store, HTTP API, static dashboard |
| `maint.py` | Maintenance windows and backed-up data purges (CLI) |
| `web/index.html`, `web/math.js` | Dashboard (no external JS/CSS) |
| `deploy/anemometer-web.service` | systemd user unit for the dashboard |
| `test_*.cpp`, `test_*.py`, `test_display.js` | Native, Python and JS tests |

## Build, test, flash

Requires the Arduino CLI with Adafruit SAMD core 1.7.17, `g++`, Python 3 with `pyserial`, and Node.

```sh
./build.sh            # compiles both roles into build/{sensor,receiver}, then runs all native and Python tests
python -m unittest test_web
node test_display.js
CLI=/home/user/pnp/Arduino/arduino-cl/bin/arduino-cli
$CLI board list       # confirm which ttyACM is which
$CLI upload -p /dev/ttyACM1 --fqbn adafruit:samd:adafruit_feather_m0 --input-dir build/sensor
$CLI upload -p /dev/ttyACM0 --fqbn adafruit:samd:adafruit_feather_m0 --input-dir build/receiver
```

* Always build with `./build.sh`: it adds `retained.ld`, without which the breadcrumb is cleared at startup (`test_trace_layout.py` checks placement).
* Upload to the real `ttyACMn`, not the by-id link. The first attempt sometimes misses the bootloader; retry.
* A board stuck in the bootloader (USB ID `239a:000b`) with intact firmware restarts with `bossac -p ttyACMn -R` (reset only, no write).
* **The host can keep listing the bootloader after a flash while the new app runs** (Nov 6, W7: about 4 min; it ended without any change to the board, so it is probably on the host/hub side). Then the port answers nothing, the LED does not breathe, and packets still arrive by radio. Check the packet log before assuming the board is dead, and don't type into a port that is really the bootloader: its command parser gets stuck until a reset.
* Calling bossac by hand: pass **`--offset=0x2000`** (the bootloader occupies the first 8 KB, write-protected). Without it, verification fails on 128 pages and the app area holds a shifted image. Double-tap reset gives a fresh bootloader (LED breathing).

## Dashboard service

```sh
cp deploy/anemometer-web.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now anemometer-web
journalctl --user -u anemometer-web -f
```

Serves `http://<host>:8765/` on all interfaces, **without authentication or TLS**, read-only except starting/stopping maintenance: trusted LAN only. The service owns the receiver port; stop it before running `python monitor.py`. History is in `data/wind.sqlite3` (not in git):

| Table | Contents | Retention |
|---|---|---|
| `samples` | One row per completed revolution received (graph points) | 7 days |
| `packets` | Every packet, heartbeats included: link continuity, reed and clock diagnostics | 7 days |
| `rain` | One row per packet whose tip count rose (`added` new tips) | 400 days |
| `runs` | One row per sensor run (session, version, first/last packet): restart warnings | 400 days |
| `gaps` | Stretches of more than 1 h without packets | 400 days |
| `weather` | Pressure (Pa) and enclosure temperature, at most one row per minute (W7) | 400 days |
| `maintenance` | Maintenance windows (start, end or NULL while open, note) | kept |
| `state` | Latest decoded packet | — |

API: `/api/live?after=<cursor>`, `/api/history?start=&end=` (≤ 12 h; with `&bucket=<s>&calm=20`, condensed, ≤ 12 days), `/api/rain?start=&end=` (≤ 32 days), `/api/weather?start=&end=` (≤ 32 days; per-bucket pressure and temperature mean/min/max, maintenance excluded), `/api/rain/total?start=&end=` (tip sum with restarts, gaps, maintenance windows and rain-logging start), `POST /api/maintenance` with JSON `{"action":"start"|"stop","note":…}` (JSON required, so another site can't trigger it). No `sqlite3` CLI is installed here; query with Python.

## Maintenance, testing and downtime

Handling can't be told apart from weather afterwards: lowering the mast spins the cups, and tipping the gauge while carrying it in looks like a downpour (15 tips in 4 min = 83 mm/h). The system has to be told instead.

* **Before touching the sensor**, press **Start maintenance** on the dashboard (or `./maint.py start --note takedown`). Everything is still recorded, but wind samples whose revolution overlaps the window, and tips inside it, are left out of graphs, strip averages and every rain total. The live cards keep showing readings, so bench tests can be watched. The window is shaded on the charts. Press **End maintenance** once the sensor is back in service. Bench tests and reflashing belong inside a window too.
* **Forgot?** Record it afterwards: `./maint.py add "2026-11-06 21:08" "21:30"` (or `open`). `list`, `end ID TIME` and `remove ID` adjust windows; removing one shows its data again. The takedown signature in `packets` is RSSI falling from its usual level (about −88 dBm at the mast) and then jumping to around −30 dBm indoors, plus a burst of rain tips.
* **Purge** only for data that should never count, such as a test against the wrong database: `./maint.py purge FROM TO` writes a consistent backup (SQLite backup API, which includes the WAL) to `data/backups/` first, then deletes `samples`, `packets` and `rain` rows in the range. Copying `wind.sqlite3` alone is not a backup while the service runs: recent writes are in `wind.sqlite3-wal`.
* Outages need no action: gaps over 1 h are recorded, shown as no data, and flagged on rain totals. A new sensor boot starts a new session, so the cumulative tip count restarts cleanly.
* Tape or wedge the rain bucket before moving the gauge.

## Measurement design

### Wind
* **Timebase:** RTC at 32 768 Hz from the crystal, extended to 64 bits in software, so intervals have no wrap limit.
* **Debounce:** each D5 change restarts a 328-tick (10.0 ms) one-shot; a level must hold that long to count. The timestamp is the last edge before the stable interval.
* **Revolutions:** the first closure after boot is the reference; closures 3, 5, 7… complete revolutions. A and B are the two half-intervals; period = A + B. No equal-spacing assumption; A/B labels can swap after a restart. A closure held at boot is not counted.
* **Speed:** Hz = 1/period of the latest *completed* revolution; RPM = 60 × Hz; knots = max(0, gain × Hz + offset), with the calibration entered per browser (wind is **uncalibrated**: a provisional 1 kn/Hz is shown with a warning).

### Rain
* Each debounced D10 closure is one tip; the reopening is ignored. The default calibration is **0.3690 mm/tip**: 2 ml per tip over a 49.5 × 109.5 mm funnel (2000 mm³ ÷ 5420.25 mm²). Check it by pouring a known volume (100 ml ≈ 50 tips ≈ 18.4 mm).
* The dashboard shows the total from local midnight to midnight, hourly bars, and rate = mm/tip ÷ max(last tip interval, time since last tip). That rate decays during dry spells and shows 0 after 1 h without a tip.
* The chart shows 1–4 days; each day's line restarts from 0 at local midnight, with the day's total above it.
* **Total since a mark** (per browser): **Mark now** (like emptying a manual gauge), a date and time, or a preset (last 1 h/24 h/72 h/7 days, month, year, water year from Oct 1). The card warns when the total is incomplete: tips not yet reported before a **sensor restart** are lost; **data gaps** over 1 h are harmless within one sensor run, because counts are cumulative; and nothing is counted before rain logging began.

### Reporting and packets
* A completed revolution or a rain tip triggers a report immediately, limited to one send per second (from completion), with a 5 s backoff after a failure and a 60 s heartbeat otherwise. Events during the cooldown are coalesced.
* **Counts are cumulative since boot**, so a lost packet loses no pulses or tips; the next packet carries the totals. Counts are RAM-only and restart with a new session ID on reset.
* **W8 payload** = W7 + `,sleep` (0 = light sleep, 1 = deep standby; worst case 223 bytes). **W7 payload** (all numbers lowercase hex, worst case 221 bytes, under the radio's 240-byte limit and the receiver's 255-character line): `W7,session,seq,count,A,B,age,rev_age,uptime,pin,pin_age,raw_edges,clock,rain_tips,rain_interval,rain_age,rain_pin,rain_raw_edges,vbat_mv,pressure_pa,temp_dk`. A, B and `rain_interval` are capped at `ffffffff` (36 h) to make room; `temp_dk` is 0.1 K (°C × 10 + 2732); both barometer fields are 0 when there is no reading. W6 is the same without the barometer fields and uncapped (worst case 235); W5 also lacks `vbat_mv`. Revolutions and period are derived by the decoder. Ages of `ffffffffffffffff` mean "never". `pin` = 1 is open, `clock` = 1 is the internal fallback. The receiver forwards `+RCV=1,len,payload,RSSI,SNR`. The decoder still accepts W1–W7.

### Battery
* `vbat_mv` is read from A7/D9 (the Feather's 1:2 battery divider): eight 12-bit reads averaged, 3.3 V full scale, sampled when each report is built. On USB it shows the charger (about 4.2 V).
* The dashboard's Battery panel shows the latest voltage, the last-6-h least-squares trend in mV/h with a rough time to 3.5 V, and a 6 h–7 day chart (mean line and min–max band). `/api/battery?start=&end=` returns per-bucket mean/min/max from the packet log (7 days).
* **Measured with W7 (light sleep only):** 4.20 → 3.82 V in 46 h outdoors (Nov 7–8), about 5–9 days per charge, matching the estimate of 5–8 mA dominated by the MCU at 48 MHz in light sleep (~4–6 mA). Radio ≈ 0.8 mA at one report per 3 s. **W8 deep standby** is expected to bring this to roughly 1 mA (6–7 weeks); compare the Battery panel's mV/h trend before and after.

### Barometer (BMP180)
* Probed at boot (chip ID 0x55, calibration EEPROM checked), read at most every 30 s inside a report at ultra-high resolution (oss 3, about 31 ms of `delay()` while counting continues in interrupts). Readings older than 5 min are sent as 0. If it is absent or keeps failing, it is probed again every 10 min, so it can be connected later.
* A watchdog reset during an I2C access (a stuck bus) disables the barometer until the next reset, using the breadcrumb (`baro` stage), so a bad I2C bus cannot cause a reset loop.
* Pressure is recorded as raw **station pressure**. The dashboard adds an optional correction and reduces to sea level with the altimeter-setting formula, so it compares directly with METAR altimeter settings. These are already reduced to sea level, so only this station's elevation matters (938 ft = 285.9 m), not the airport's. Check, Nov 6 23:34: KARB 30.03 inHg implies 982.93 hPa here, and the sensor read 982.93 hPa, so the default correction is 0. Settings has a "Calibrate to it now" helper; repeat on a few days (METARs are rounded to 0.34 hPa), especially once the sensor is outside and colder.
* The dashboard shows pressure, its 3 h tendency, temperature, and 6 h–30 day charts. `!i2c-scan` lists I2C devices; `!status` includes a `baro` line.

### Dashboard display rules
* Gaps are not zero wind: lines break only on missing revolutions, sensor restarts or no data.
* **Deduced calm (display only):** a half-revolution longer than 20 s means < 1.5 RPM, below resolution. The page draws 0 (hollow dots, dashed line) from 20 s after that half began until the closure that ended it, instead of the spanning revolution's meaningless average. A provisional zero is drawn live. A dashed zero during obvious wind points to a missed closure (wiring), not calm.
* **Calm on the live cards:** the same argument applies to the current reading. Speed and RPM show 0, and the period shows "> N s", once no closure for 20 s is *certain*. The sensor reports a middle closure only with the next heartbeat, so after a completed revolution that takes 40 s; after a known middle closure, 20 s (plus 3 s latency margin). The status then reads "Calm · no closure for …". A stale radio link is never treated as calm.
* **Link and sensor card:** RSSI and SNR (latest, 1 h average and minimum), last-packet age and sequence, missed packets over 1 h and 24 h (sequence numbers skipped within each sensor session), battery (W6), 32 kHz clock source, both reed states, protocol, session and uptime, malformed frames. Link statistics include maintenance time and are cached for 20 s.
* During maintenance the cards keep showing live readings; only graphs, averages and rain totals leave the data out.
* Each strip's right column shows max, time-weighted average (revolutions ÷ time, calm = 0), min, n and calm time; min/max come from real points.
* Strips can be 10 s to 24 h long, aligned to local time (24 h strips start at local midnight; a strip spanning a DST change is offset by an hour). Strips over 1 h load **condensed** history: per bucket (strip ÷ 720), the slowest and fastest revolution, so extremes and calms survive, plus revolution and calm totals for an exact strip average. Dots are drawn only while they fit (about one per 4 px).
* The balance between half-turns (A vs B) is not shown: reed chatter makes it too erratic to use until the hardware is improved. A and B are still listed, because a very short one points to chatter.

## Lessons learned (why the firmware looks like this)

* **Never block forever.** The original sensor hung in `Serial1.flush()`. Adafruit SAMD core 1.7.17 acknowledges UART errors with a read-modify-write of the write-1-to-clear `INTFLAG`, which can clear TXC, so the wait for TXC never ended. `RadioUart` bypasses the TX ring, waits for DRE, then for TXC **or** two character times (`txc_inferred` counts the latter), with deadlines throughout. RTC syncs are also bounded, and a 16 s watchdog (GCLK4 = OSCULP32K) backs everything up.
* **RTC compare fires only on equality.** If COUNT passes COMP during the write sync, the next match is 2³² ticks (~36 h) away. `debounceArm()` uses a margin, re-reads COUNT after writing, and re-arms (`compare_rearms`).
* **The 32 kHz crystal can stop** (condensation on the PCB, Nov 3: 3 h outage). The RTC and reed EIC froze while the CPU ran, so the watchdog never fired, and a reset would not help because startup waits for the crystal. The firmware now detects 8 s without an RTC interrupt, moves RTC/EIC to the internal OSC32K (GCLK5), adds the frozen time back from `millis()`, retries the crystal every 10 min, and flags `clock=1`. Never write RTC registers, or read `COUNT`, while a sync is stuck: it stalls the bus until the watchdog resets (W8-standby1/2 read `COUNT` after a timed-out sync; fixed in standby3). Keep the PCB dry (sealed enclosure, desiccant, conformal coat).
* **Radio recovery:** after 3 consecutive failed reports the sensor pulses RYLR998 NRST on D6 and reconfigures it. Do not query the radio's mode right after `AT+MODE=1`: the query wakes it.
* **Reed wiring is the usual fault.** Gaps that ended in one long half-interval were the reed or its wires, not the firmware. The raw-edge counters separate the cases: `pin=1` with no new edges is open or disconnected; `pin=0` with no new edges is held closed; raw edges rising faster than 2 per counted closure is chatter. Chatter that survives the 10 ms debounce shows up as "double-speed" readings with a short A or B.
* **Standby (W8):** the sensor enters MCU deep standby between events unless a USB host was seen this session (checked 1.5 s after boot and at each report via USB frame numbers); then it stays in light sleep so USB and `!status` keep working. To get deep standby after a USB session, unplug USB and press reset. A host cannot enumerate the sensor while it is in deep standby (it gives up with "device not accepting address"), so plugging USB into a running battery sensor needs a reset too. The `sleep` packet field and the dashboard's Sleep row show the mode. In standby `millis()` stops, so everything that must count time across sleep (crystal retry, barometer spacing, `!xosc-test`) uses RTC time, and a stopped crystal, which also stops the RTC and EIC wake-ups, is caught by the WDT early-warning interrupt 8 s after the last kick (`wdt_early` in `!status`), well before the 16 s reset. Reed edges wake via `EIC.WAKEUP`. SysTick's interrupt is masked during standby, as ArduinoLowPower does, and the SAMD21 flash-standby erratum workaround (`NVMCTRL.CTRLB.SLEEPPRM = DISABLED`) is kept. `standby=` in `!status` counts entries/returns.
* **Breadcrumbs:** `Diagnostics.h` records the current execution stage in `.noinit` RAM. After an unexplained reset, read `!status` *before* any other reset to get `previous_trace` (not valid after power loss). Reset cause bits: POR 0x01, BOD12 0x02, BOD33 0x04, external 0x10, watchdog 0x20, software 0x40.
* **USB replies are gated:** the firmware only writes to USB after seeing USB frames, rechecked about every 5 s. Wait about 7 s after opening the port before sending a command.
* **Keep the project in git with a remote.** The working directory was once deleted with `rm -rf`; it was rebuilt from session transcripts and the Arduino build cache, and two tests had to be rewritten.

## USB commands (sensor, 115200 baud, newline terminated)

| Command | Effect |
|---|---|
| `!status` | Counts, raw D5/D10 state and edges, radio/UART/RTC/clock counters, reset cause, previous-boot breadcrumb |
| `!report` | Send a report now (bypasses the cooldown) |
| `!uart-test` | Inject one UART completion timeout; expect recovery and a retry about 5 s later |
| `!xosc-test` | Stop the crystal for 90 s; expect immediate failover to the internal clock (`rtc_sync_timeouts` +1) |
| `!xosc-test deep` | The same in forced deep standby: USB detaches and returns, in light sleep, once the crystal is restored (next retry, up to ~10 min after failover) |
| `!clock-retry` | Retry the crystal now |
| `!standby-test` | Say whether deep standby is active this session |
| `!diag-reset` | Software reset to test breadcrumb retention (**resets counts**) |
| `!i2c-scan` | List devices on the I2C bus (BMP180 = 0x77) |
| `!led on` / `!led off` | Identify the board |

## Open items

* Wind calibration (gain/offset) has not been measured.
* The rain calibration is a geometric estimate; verify with a known volume.
* W8 deep standby is built and unit-tested but not yet soaked on hardware: after flashing, confirm on battery that packets, counts, the barometer and clock fallback behave, and measure the new drain rate.
* RF margin outdoors was marginal at times (RSSI ≈ −100 dBm); there is no acknowledgement or retransmit beyond cumulative counts.
