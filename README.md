# wx-link-test: Feather M0 / RYLR998 weather link

A battery sensor counts anemometer and tipping-bucket rain-gauge reed closures, reads a BMP180 barometer and an SHT31
temperature/humidity sensor, and reports over LoRa. A USB-attached receiver forwards packets to a host, which records
them in SQLite and serves a live LAN dashboard.

The sensor runs firmware **W9-sht31** on a Feather M0 Adalogger (bench-tested 2026-11-17; not yet redeployed). The
receiver runs the same sketch built with `SENSOR=0`.

## Where things are documented

| Topic | Where |
|---|---|
| Pinout, radio settings, packet fields, timing, USB commands, datasheets | Dashboard reference page: `http://<host>:8765/reference.html` (source `web/reference.html`) |
| How each quantity is measured and displayed; spin-down tests | [docs/design.md](docs/design.md) |
| Why the firmware looks the way it does | [docs/lessons.md](docs/lessons.md) |
| Current work list and bench instructions | [docs/plan.md](docs/plan.md) |
| W8 outdoor deployment record (first sensor board) | [docs/feather-m0-baseline.md](docs/feather-m0-baseline.md) |
| nRF52840 port (parked; code on branch `nrf52840-port`) | [docs/nrf52840-port-plan.md](docs/nrf52840-port-plan.md) |

## Hardware

| Role | Board | USB serial ID | Bootloader (USB ID) | LoRa address |
|---|---|---|---|---|
| Sensor | Feather M0 Adalogger | `BD0F94DC50305841502E3120FF0B0E2F` | UF2 (`239a:0015`) | 1 |
| Receiver | Feather M0 | `B55AB548514D4D5447202020FF012F18` | SAM-BA (`239a:000b`) | 2 |

Use `/dev/serial/by-id/usb-Adafruit_Feather_M0_<ID>-if00` for monitoring; `ttyACMn` numbers change. The first sensor
board (`8FFE7CD9…`) is retired.

Sensor wiring in brief: wind reed D5, rain reed D10 (both to GND, internal pull-ups), RYLR998 on D0/D1 with NRST on
D6, BMP180 (0x77) and SHT31 (0x44) on SDA/SCL, battery on the JST connector. Do not use D9 (battery divider) or A0
(session-ID entropy). The full table is on the reference page.

Radio: 915 MHz, SF7, BW 500 kHz, CR 4/5, preamble 12, network 18, 2 dBm (configured and read back at startup; not a
regulatory assessment).

## Layout

| Path | Purpose |
|---|---|
| `link/link.ino` | Firmware for both roles (`-DSENSOR=1/0`) |
| `link/PulseCounter.h`, `link/TipCounter.h` | Wind and rain reed debounce and counting |
| `link/ReportSchedule.h` | Report triggering, 1 s cooldown, 5 s backoff, 60 s heartbeat |
| `link/Bmp180.h`, `link/Sht31.h` | Sensor maths and CRC (no I/O; tested natively) |
| `link/SoftI2c.h` | Software I2C master with timeouts (no hardware access; tested natively) |
| `link/Sensors.h`, `link/UsbCommands.h` | Parts of the sketch: sensor services and I2C diagnostics; USB command parser and `!status` |
| `link/RadioUart.h` | DMA UART transmit to the RYLR998, bounded waits |
| `link/Diagnostics.h`, `retained.ld` | Execution-stage breadcrumb kept in `.noinit` RAM across resets |
| `monitor.py` | Packet decoder (W1–W9) and terminal monitor |
| `web_server.py` | Serial reader, SQLite store, HTTP API, static pages |
| `maint.py`, `fault.py` | Maintenance windows and purges; per-sensor fault periods (CLI) |
| `backup.py` | Consistent nightly copy of the database |
| `spindown.py`, `spindown_sets.json` | Bearing spin-down analysis |
| `web/` | Dashboard (`index.html`, `app.js`, `math.js`), reference page, spin-down page (no external JS/CSS) |
| `deploy/` | systemd user units |
| `test_*` | Native C++, Python and JS tests |

## Build, test, flash

Requires the Arduino CLI with Adafruit SAMD core 1.7.17, `g++`, Python 3 with `pyserial`, and Node.

```sh
./build.sh            # compiles both roles into build/{sensor,receiver}, then runs every test (C++, Python, JS)
CLI=/home/user/pnp/Arduino/arduino-cl/bin/arduino-cli
$CLI board list       # confirm which ttyACM is which
$CLI upload -p /dev/ttyACM1 --fqbn adafruit:samd:adafruit_feather_m0 --input-dir build/sensor
$CLI upload -p /dev/ttyACM0 --fqbn adafruit:samd:adafruit_feather_m0 --input-dir build/receiver
```

* Always build with `./build.sh`: it adds `retained.ld`, without which the breadcrumb is cleared at startup.
* Upload to the real `ttyACMn`, not the by-id link. The first attempt sometimes misses the bootloader; retry.
* **Sensor board (UF2 bootloader):** this host must ignore the bootloader's USB drive or flashing fails with
  "No device found". Once per boot: `echo 239a:0015:i | sudo tee /sys/module/usb_storage/parameters/quirks`, or
  permanently `options usb-storage quirks=239a:0015:i` in `/etc/modprobe.d/feather-uf2.conf`.
* **After flashing the sensor it often does not come back on USB** and drops into deep standby. Packets still arrive
  by radio. Press reset once with USB connected to get the USB port (and light sleep) back.
* Manual flash from a breathing-LED bootloader (double-tap reset):
  `~/.arduino15/packages/adafruit/tools/bossac/1.8.0-48-gb176eee/bossac -i -d --port=ttyACM1 -U -e -w -v --offset=0x2000 -R build/sensor/link.ino.bin`.
  **`--offset=0x2000` is required** (the bootloader occupies the first 8 KB).
* The host can keep listing the bootloader after a flash while the new app runs. Check the packet log before assuming
  the board is dead, and don't type into a port that is really the bootloader.
* USB replies are gated: wait about 7 s after opening the port before sending a command such as `!status`.

## Dashboard service

```sh
cp deploy/anemometer-*.service deploy/anemometer-*.timer ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now anemometer-web anemometer-backup.timer
journalctl --user -u anemometer-web -f
```

Serves `http://<host>:8765/` on all interfaces, **without authentication or TLS**, read-only except starting/stopping maintenance: trusted LAN only. The service owns the receiver port; stop it before running `python monitor.py`. History is in `data/wind.sqlite3` (not in git). `backup.py`, run nightly at 03:30 by `anemometer-backup.timer`, writes a consistent copy to `data/backups/nightly/` and keeps 14. `/api/hourly?start=&end=` returns the long-term hourly summary.

Tables:

| Table | Contents | Retention |
|---|---|---|
| `samples` | One row per completed revolution received (graph points) | 7 days |
| `packets` | Every packet, heartbeats included: link continuity, reed and clock diagnostics | 7 days |
| `rain` | One row per packet whose tip count rose (`added` new tips) | 400 days |
| `runs` | One row per sensor run (session, version, first/last packet): restart warnings | 400 days |
| `gaps` | Stretches of more than 1 h without packets | 400 days |
| `weather` | Pressure (Pa) and enclosure temperature, at most one row per minute (W7) | 400 days |
| `climate` | SHT31 air temperature and relative humidity, at most one row per minute (W9) | 400 days |
| `hourly` | Per clock hour: packets received and missed, sessions, battery mean/min/max, RSSI and SNR mean/min, packets sent in deep standby. Filled from `packets` before they are pruned | 400 days |
| `maintenance` | Maintenance windows (start, end or NULL while open, note) | kept |
| `faults` | Sensor faults (sensor wind/rain/baro/air, start, end or NULL while open, note) | kept |
| `state` | Latest decoded packet | — |

API: `/api/live?after=<cursor>`, `/api/history?start=&end=` (≤ 12 h; with `&bucket=<s>&calm=20`, condensed, ≤ 12 days), `/api/rain?start=&end=` (≤ 32 days), `/api/weather?start=&end=` (≤ 32 days; per-bucket pressure and temperature mean/min/max, maintenance excluded), `/api/rain/total?start=&end=` (tip sum with restarts, gaps, maintenance windows and rain-logging start), `POST /api/maintenance` with JSON `{"action":"start"|"stop","note":…}` (JSON required, so another site can't trigger it). No `sqlite3` CLI is installed here; query with Python.

## Maintenance, testing and downtime

Handling can't be told apart from weather afterwards: lowering the mast spins the cups, and tipping the gauge while carrying it in looks like a downpour (15 tips in 4 min = 83 mm/h). The system has to be told instead.

* **Before touching the sensor**, press **Start maintenance** on the dashboard (or `./maint.py start --note takedown`). Everything is still recorded, but wind samples whose revolution overlaps the window, and tips inside it, are left out of graphs, strip averages and every rain total. The live cards keep showing readings, so bench tests can be watched. The window is shaded on the charts. Press **End maintenance** once the sensor is back in service. Bench tests and reflashing belong inside a window too.
* **Forgot?** Record it afterwards: `./maint.py add "2026-11-06 21:08" "21:30"` (or `open`). `list`, `end ID TIME` and `remove ID` adjust windows; removing one shows its data again. The takedown signature in `packets` is RSSI falling from its usual level (about −88 dBm at the mast) and then jumping to around −30 dBm indoors, plus a burst of rain tips.
* **Sensor faults:** when one sensor gives wrong data while the others are fine (e.g. a disconnected anemometer wire reads as calm), mark it with `fault.py`: `./fault.py add wind "2026-11-12 20:06" "22:10:44" --note "wire off"`, or `start wind` / `stop wind` while it is happening; `list`, `end ID TIME` and `remove ID` as for maintenance. Sensors are `wind`, `rain`, `baro` and `air`. That sensor's data is left out of its graphs, averages and totals only; the dashboard shows a red banner and status while a fault is open and red shading afterwards. There is no dashboard button yet.
* **Purge** only for data that should never count, such as a test against the wrong database: `./maint.py purge FROM TO` writes a consistent backup (SQLite backup API, which includes the WAL) to `data/backups/` first, then deletes `samples`, `packets` and `rain` rows in the range. Copying `wind.sqlite3` alone is not a backup while the service runs: recent writes are in `wind.sqlite3-wal`.
* Outages need no action: gaps over 1 h are recorded, shown as no data, and flagged on rain totals. A new sensor boot starts a new session, so the cumulative tip count restarts cleanly.
* Tape or wedge the rain bucket before moving the gauge.

## Open items

See [docs/plan.md](docs/plan.md) for the work list. In short: reeds not yet wired on the new board; no current
measured with a meter; wind calibration weak above 4 kt; rain calibration is a geometric estimate; no acknowledgement
or retransmit beyond cumulative counts.
