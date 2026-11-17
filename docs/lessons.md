# Lessons learned

Why the firmware and hardware look the way they do. Newest first within each part.

## Rebuild on a new Feather M0 Adalogger (Nov 16–17)

* **The Arduino `Wire` driver on SAMD21 has no timeouts.** With a faulty SHT31 on the bus it hung in `setup()`, before the watchdog was running, and the board stayed dead until reset. The sensors are now driven by a software I2C master with timeouts, the watchdog starts first, and the bus lines are checked at boot.
* **A sensor can answer and still be bad.** The first SHT31 acknowledged its address and returned a valid status word with a correct CRC, but never completed a measurement (still busy after 100 ms; the limit is 15.5 ms). A replacement worked at once. Buy sensors from a distributor, and test a measurement, not just the address.
* **Connectors back out.** The SHT31's connector had worked loose during the same session. With the Nov 12 wire and the Nov 13 chatter, every wiring fault so far has been a connection. Use locking or soldered connections with strain relief.
* **The new board's UF2 bootloader stalls USB on this host.** Its drive (`FEATHERBOOT`, USB `239a:0015`) never answers the PC's first read; the kernel resets the device every 30 s and the bootloader's serial port is dead meanwhile, so `bossac` finds no device. Fix: `echo 239a:0015:i | sudo tee /sys/module/usb_storage/parameters/quirks` (lost at reboot), or permanently `options usb-storage quirks=239a:0015:i` in `/etc/modprobe.d/feather-uf2.conf`.
* **After a flash the new board often does not re-enumerate on USB**, so it sees no host, enters deep standby, and needs a reset press to get USB back. Packets still arrive by radio, so check them before assuming a failed flash. Cause (Nov 17): after a software reset (flash, `!diag-reset`) this host, through its VIA 2109:2813 hub, logs no disconnect at all, even with a 1 s `USBDevice.detach()` in `setup()`. It re-enumerates only after the port is opened and closed (`cdc_acm` close, within ~6 s); a sensor in deep standby cannot answer then, and the host gives up. Fix: the sensor holds light sleep for 3 minutes after a software reset (`SYST_LIGHT_MS`), and the 1 s detach was removed. Recovery: open and close the port (any `!` command does it) and wait a few seconds. Untested: whether a root port (no hub) notices the reset by itself.
* **Software I2C pin order (F1, Nov 17):** `pinMode(OUTPUT)` before `digitalWrite(LOW)` drives the line high for an instant when coming from `INPUT_PULLUP` (OUT=1 selects the pull-up). The driver now uses direct port writes: OUT cleared before the pin becomes an output, input released before the pull-up is re-enabled. 40 of 40 `!i2c-probe` runs clean.

## nRF52840 Pro Micro clone (Nov 15–16, parked on branch `nrf52840-port`)

The nRF52840 port is parked on branch `nrf52840-port` (the Pro Micro clone was hard to work with: a pulled-up VCC
enable that costs ~640 µA to hold off, ~110 µA sleep floor with nothing attached, and a bootloader that ignores
software DFU requests). Production may return to the nRF52840 on a custom board. Meanwhile the sensor is being
rebuilt on a **new Feather M0** with the parts in hand, in a new enclosure. Fast (≤ 1 s) reports stay for development.

Details and measurements are in [nrf52840-port-plan.md](nrf52840-port-plan.md).

## Carried into the rebuild

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

## Feather M0 firmware (W1–W8)

* **Never block forever.** The original sensor hung in `Serial1.flush()`. Adafruit SAMD core 1.7.17 acknowledges UART errors with a read-modify-write of the write-1-to-clear `INTFLAG`, which can clear TXC, so the wait for TXC never ended. `RadioUart` bypasses the TX ring, waits for DRE, then for TXC **or** two character times (`txc_inferred` counts the latter), with deadlines throughout. RTC syncs are also bounded, and a 16 s watchdog (GCLK4 = OSCULP32K) backs everything up.
* **RTC compare fires only on equality.** If COUNT passes COMP during the write sync, the next match is 2³² ticks (~36 h) away. `debounceArm()` uses a margin, re-reads COUNT after writing, and re-arms (`compare_rearms`).
* **The 32 kHz crystal can stop** (condensation on the PCB, Nov 3: 3 h outage). The RTC and reed EIC froze while the CPU ran, so the watchdog never fired, and a reset would not help because startup waits for the crystal. The firmware now detects 8 s without an RTC interrupt, moves RTC/EIC to the internal OSC32K (GCLK5), adds the frozen time back from `millis()`, retries the crystal every 10 min, and flags `clock=1`. Never write RTC registers, or read `COUNT`, while a sync is stuck: it stalls the bus until the watchdog resets (W8-standby1/2 read `COUNT` after a timed-out sync; fixed in standby3). Keep the PCB dry (sealed enclosure, desiccant, conformal coat).
* **Radio TX goes out by DMA (W8-uart1).** The RYLR998 ends a command at an idle gap of about one character (87 µs at 115200). With CPU-written bytes, reed/RTC interrupts (several RTC syncs each) split lines into `+ERR=1` / `+ERR=2` pairs, and the low-priority RX ISR was overrun (`+O`): about 10% of sends failed while the rotor turned (Nov 9 bench; also W7's ~1% loss in wind). Now DMA channel 0 feeds SERCOM0 on DRE, and the SERCOM0 RX ISR (priority 0) preempts EIC/RTC (priority 1). Bench after the fix: 0 errors in 139 sends while turning. `tx_dma_timeouts` is in `!status`.
* **Radio recovery:** after 3 consecutive failed reports the sensor pulses RYLR998 NRST on D6 and reconfigures it. Do not query the radio's mode right after `AT+MODE=1`: the query wakes it.
* **Reed wiring is the usual fault.** Gaps that ended in one long half-interval were the reed or its wires, not the firmware. The raw-edge counters separate the cases: `pin=1` with no new edges is open or disconnected; `pin=0` with no new edges is held closed; raw edges rising faster than 2 per counted closure is chatter. Chatter that survives the 10 ms debounce shows up as "double-speed" readings with a short A or B.
* **Standby (W8):** the sensor enters MCU deep standby between events unless a USB host was seen this session (watched for up to 10 s after boot, then at each report, via USB frame numbers; after a flash this host can take several seconds to re-enumerate); then it stays in light sleep so USB and `!status` keep working. To get deep standby after a USB session, unplug USB and press reset. A host cannot enumerate the sensor while it is in deep standby (it gives up with "device not accepting address"), so plugging USB into a running battery sensor needs a reset too. The `sleep` packet field and the dashboard's Sleep row show the mode. In standby `millis()` stops, so everything that must count time across sleep (crystal retry, barometer spacing, `!xosc-test`) uses RTC time, and a stopped crystal, which also stops the RTC and EIC wake-ups, is caught by the WDT early-warning interrupt 8 s after the last kick (`wdt_early` in `!status`), well before the 16 s reset. Reed edges wake via `EIC.WAKEUP`. SysTick's interrupt is masked during standby, as ArduinoLowPower does, and the SAMD21 flash-standby erratum workaround (`NVMCTRL.CTRLB.SLEEPPRM = DISABLED`) is kept. `standby=` in `!status` counts entries/returns.
* **Breadcrumbs:** `Diagnostics.h` records the current execution stage in `.noinit` RAM. After an unexplained reset, read `!status` *before* any other reset to get `previous_trace` (not valid after power loss). Reset cause bits: POR 0x01, BOD12 0x02, BOD33 0x04, external 0x10, watchdog 0x20, software 0x40.
* **USB replies are gated:** the firmware only writes to USB after seeing USB frames, rechecked about every 5 s. Wait about 7 s after opening the port before sending a command.
* **Keep the project in git with a remote.** The working directory was once deleted with `rm -rf`; it was rebuilt from session transcripts and the Arduino build cache, and two tests had to be rewritten.
