# Work plan and handoff (written 2026-11-17, 01:45)

Full review: "Weather Station: Review and Cleanup Plan" (Claude doc, https://claude.ai/artifact/EWddUXAxh16S41YgY4t17A).
Item numbers (F1, D2, H1...) refer to it.

## Done

- **Pass 1, documents:** README rewritten for W9 and the new sensor board; `docs/design.md`, `docs/lessons.md`;
  baseline doc closed as the W8 record; reference page corrected; nRF52840 bring-up results in the port plan.
- **Pass 2, structure (no behaviour change):** `link/SoftI2c.h` + `test_softi2c.cpp`, `link/Sensors.h`,
  `link/UsbCommands.h`; `ST_BARO` -> `ST_I2C`, `baroDisabled` -> `i2cOff`; one version list in `monitor.py`;
  dashboard script in `web/app.js`; `test_reference.py`; `./build.sh` runs every test.
- **Pass 4, host:** `hourly` table and `/api/hourly` (backfilled from Nov 10 01:00; earlier packets were already
  pruned); `backup.py` with `anemometer-backup.timer` installed and enabled (03:30 nightly, 14 kept).

## State of the bench at handoff

- **The sensor board still runs the pre-split firmware** (commit cfe50f6 build). The restructured firmware builds and
  passes tests but **has not been flashed**. Flash it first and confirm packets still carry BMP180 and SHT31 values.
- Sensor was on USB in light sleep (session `47f976da`). Maintenance window 6 is open.
- `usb_storage` quirk for 239a:0015 is set until the PC reboots (see README, flashing).

## To do, in order

### A. Small leftovers (no bench)
1. (Done) README and reference page cover the hourly table and backups.
2. (Done) Stray files in the repo root deleted (Nov 17).
3. `link.ino` is 690 lines (target was 500). Further split is optional: clock/RTC code is the next candidate.
4. (Done) D8: usb-storage quirk for 239a:0015 made permanent in `/etc/modprobe.d/` (Nov 17).

### B. Pass 3, firmware fixes on the bench (good SHT31 fitted)
Flash with `arduino-cli upload -p /dev/ttyACM1 --fqbn adafruit:samd:adafruit_feather_m0 --input-dir build/sensor`.
If the board does not return on USB afterwards, press reset once. Wait 7 s after opening the port before a command.

Done on Nov 17 (05:00-05:40):

1. (Done) Flashed `main`: first packet of the new session carried pressure, air temp and RH; `!status` clean.
2. (Done) **F1:** `I2cPins` uses direct port writes. `!i2c-probe` 40/40 clean (0x44, 0x77 ACK, SHT31 status CRC OK).
3. (Done) **F2:** 24 software resets, SDA/SCL read high at every boot; no false "held low". The 1-minute re-check
   stays as the only safeguard. 21 of 22 first packets after `!diag-reset` arrived.
4. (Done) **F4, F5:** SHT31 poll 10 x 10 ms; re-probe after 3 failed reads in a row; probe reads status first and
   sends Break + soft reset only if that fails.
5. (Done) **F6:** `i2cOffForBoot` removed. A reset during I2C starts with I2C off and re-checks after 10 minutes
   (lines low: 1 minute), so a bus that really hangs costs one reset per 10 minutes, not one per minute.
6. (Done) **F3:** the host (sensor is behind a VIA 2109:2813 hub) logs no disconnect for a software reset,
   even with the 1 s detach. It re-enumerates after the port is opened and closed. A board in deep standby can't
   answer then. Fix: hold light sleep for 3 minutes after a software reset (`SYST_LIGHT_MS`); the detach was
   removed. Over 22 resets the port came back after 1-9 pokes (3.5 s to 2.5 min). Nov 17 evening: on a PC port (internal Genesys 05e3:0608 hub, 1-11.4) every `!diag-reset`
   (5/5) was seen at once and re-enumerated in 0.77 s with no poke; same on a root port (1-8, no hub), 5/5 in 0.78 s. **The VIA hub is the cause: keep the sensor
   off it.** The light-sleep hold stays as a cheap safeguard.
7. (Done) Final build flashed 05:33 (session bb3ed4fa), 11/11 packets in 10 minutes, tagged `feather-m0-w9`.

### C. Rebuild and deployment (needs hands)
1. (Wired, Nov 18) Wind reed (D5) and rain reed (D10) with filters. Hand test 00:11: 59 pulses = 118 edges, 10 tips =
   20 edges, all reaching the host; two earlier runs showed bounce (~1 extra edge per 1.5 pulses) and one with no rain
   edges. Still to do: wiggle test with `raw_edges` in `!status`.
2. D2: meter the calm floor at the battery lead (unplug USB, then reset) and the RYLR998 alone (expect ~10 uA).
3. New bearing housing if ready; repeat the spin-down.
4. (Deployed Nov 18, window 6 closed 16:13, **400 mAh LiPo** in place of the 1200 mAh.) 30-day soak: at the old
   mixed rate (~1.5-2 mA) one charge lasts about 1.5 weeks, so plan recharges or a bigger cell. Judge it from `/api/hourly`.
5. Calibrate against the handheld in a steady wind.

### D. Later
W10 (10 s reports with gust tracking; AS5600 vane on A1/D11), then the production-board decision.
