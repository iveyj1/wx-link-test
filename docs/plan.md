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
2. Untracked files in the repo root, left alone: `notes` (already folded into `docs/lessons.md`), `pinout` (now on the
   reference page), `agent`, `cl`, `move-session.sh`. Delete or keep as you prefer.
3. `link.ino` is 690 lines (target was 500). Further split is optional: clock/RTC code is the next candidate.
4. D8 (needs sudo): `echo 'options usb-storage quirks=239a:0015:i' | sudo tee /etc/modprobe.d/feather-uf2.conf`.

### B. Pass 3, firmware fixes on the bench (good SHT31 fitted)
Flash with `arduino-cli upload -p /dev/ttyACM1 --fqbn adafruit:samd:adafruit_feather_m0 --input-dir build/sensor`.
If the board does not return on USB afterwards, press reset once. Wait 7 s after opening the port before a command.

1. **Flash the current `main`** and check packets (pressure, air temp, RH present) and `!status`.
2. **F1:** in `link.ino` `I2cPins::low()`, replace `pinMode(OUTPUT); digitalWrite(LOW)` with direct port writes:
   `PORT->Group[port].OUTCLR.reg = mask; PORT->Group[port].PINCFG[pin].reg = PORT_PINCFG_INEN; PORT->Group[port].DIRSET.reg = mask;`
   (and `release()` as DIRCLR, then PINCFG = INEN | PULLEN, then OUTSET). Run `!i2c-probe` 20 times: expect
   `0x44: ACK`, `0x77: ACK`, SHT31 status CRC OK every time, no FAULT line. Then correct the comment.
3. **F2:** print the raw SDA/SCL reads from `i2cLinesCheck()` at boot (it already logs `# i2c ...`); reset 20 times
   and note any "held low". None in 20 -> leave the 1-minute re-check as the only safeguard and say so in the comment.
4. **F4, F5** (`link/Sensors.h`): `shtRead()` poll limit 150 x 10 ms -> 10 x 10 ms; after 3 consecutive failures set
   `shtAddr = 0` so the 1-minute probe takes over. In `shtBegin()`, send Break + soft reset only when the first
   status read fails.
5. **F6:** drop `i2cOffForBoot`: after a reset during I2C, start with I2C off and let `i2cReady()` re-check after a
   minute like the low-lines case.
6. **F3:** find why USB does not re-enumerate after a flash (suspect: host sees no disconnect; the 1 s
   `USBDevice.detach()` in `setup()` did not cure it). Fallback: stay in light sleep for 60 s after a software reset
   (`resetCause & PM_RCAUSE_SYST`) before allowing deep standby, and remove the 1 s detach if it does not help.
7. `./build.sh`, flash, 10 minutes of packets, then `git tag feather-m0-w9 && git push --tags`.

### C. Rebuild and deployment (needs hands)
1. Wire wind reed (D5) and rain reed (D10) with 1 k + 100 nF filters; wiggle test with `raw_edges` in `!status`.
2. D2: meter the calm floor at the battery lead (unplug USB, then reset) and the RYLR998 alone (expect ~10 uA).
3. New bearing housing if ready; repeat the spin-down.
4. Deploy; `./maint.py stop` (closes window 6); 30-day soak. Judge it from `/api/hourly`.
5. Calibrate against the handheld in a steady wind.

### D. Later
W10 (10 s reports with gust tracking; AS5600 vane on A1/D11), then the production-board decision.
