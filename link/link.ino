#include <Arduino.h>
#include "PulseCounter.h"
#include "TipCounter.h"
#include "ReportSchedule.h"
#include "Diagnostics.h"
#include "RadioUart.h"
#include "Bmp180.h"
#include <Wire.h>
#ifndef SENSOR
#define SENSOR 1
#endif

#define FIRMWARE "W8-standby3"
constexpr uint32_t HZ = 32768;
constexpr uint8_t REED = 5;
constexpr uint8_t VBAT = A7; // = D9: battery through the Feather's 1:2 divider (do not use D9 otherwise)
constexpr uint8_t RAIN = 10; // D10 = PA18, EXTINT2: tipping-bucket reed to GND (internal pull-up); ~100 ms closure per tip
constexpr uint8_t RADIO_RESET = 6; // D6 -> RYLR998 NRST (active low); harmless if unwired
constexpr uint32_t RADIO_RESET_AFTER_FAILS = 3;
PulseCounter pulses;
TipCounter rain;
volatile uint32_t rtcHigh = 0;
volatile bool reportDue = false;
uint64_t nextReport = 5 * uint64_t(HZ);
bool radioOK = false, usbLive = true;
uint32_t session, sequence = 0;
ReportSchedule reports;
bool reporting = true, forceReport = false;
char radioLine[256];
size_t radioLen = 0;
RadioUart radioUart;
uint8_t resetCause = 0;
uint32_t standbyEntries = 0, standbyReturns = 0, txOK = 0, txFailed = 0;
uint64_t txLedUntil = 0;
// Raw D5 activity, independent of debouncing: separates stuck-open, stuck-closed and chatter.
uint64_t rawEdges = 0, lastRawChange = 0, rainRawEdges = 0;
uint32_t rtcSyncTimeouts = 0, compareRearms = 0, radioResets = 0, consecutiveFails = 0;

// 32.768 kHz crystal failure handling. The RTC and reed EIC run from GCLK2 = XOSC32K.
// Condensation stopped the crystal for 3 h on Nov 3: RTC/EIC froze, CPU kept running.
// Fallback: GCLK5 = internal OSC32K (factory calibrated, less accurate), selected per peripheral.
constexpr uint8_t GCLK_CRYSTAL = 2, GCLK_INTERNAL = 5;
constexpr uint32_t RTC_QUIET_MS = 8000;      // RTC compare fires at least every 5 s when healthy
constexpr uint32_t CRYSTAL_RETRY_MS = 600000; // try the crystal again every 10 min
volatile uint32_t lastRtcIrqMs = 0;
volatile uint64_t lastRtcIrqTicks = 0;
// Once a sync times out, further RTC register writes would stall the bus until the clock
// returns. While rtcStuck, ticksLocked() returns the last value and nothing touches the RTC.
volatile bool rtcStuck = false;
uint64_t lastTicks = 0;
bool onInternalClock = false;
uint32_t clockFailovers = 0, clockRestores = 0, rtcStallRearms = 0, clockLostMs = 0;
uint32_t lastCrystalTryMs = 0, xoscTestUntil = 0; // RTC wall milliseconds (see wallMs())
// Deep standby (W8): on unless a USB host has been seen this session. In standby the CPU,
// 48 MHz DFLL and SysTick stop, so millis() stands still; the RTC, the reed EIC and the WDT
// keep running and wake the CPU. A stopped crystal also stops the RTC and EIC, so the WDT
// early-warning interrupt (8 s after the last kick, on OSCULP32K) wakes the clock monitor.
constexpr uint32_t WDT_EARLY_MS = 8000;
bool deepSleep = false, usbSeen = false;
// !xosc-test deep detaches USB so the host cannot switch the sensor back to light sleep
// mid-test; it re-attaches once the crystal is restored.
bool usbDetachedForTest = false, testForcesDeep = false;
volatile bool wdtEarly = false;
volatile uint32_t wdtEarlyMs = 0;
uint32_t wdtEarlyWakes = 0;

void rtcSync() {
  StageScope stage(ST_CLOCK_SYNC);
  // Runs with interrupts masked, so millis() cannot bound it. Normal sync is a few
  // 32 kHz cycles (~1.5k iterations); give up after a few ms rather than lock up.
  for (uint32_t n = 0; RTC->MODE0.STATUS.bit.SYNCBUSY; )
    if (++n > 50000) { ++rtcSyncTimeouts; rtcStuck = true; break; }
}
// Caller masks interrupts (or is in a non-preemptible ISR).
uint64_t ticksLocked() {
  if (rtcStuck) return lastTicks;
  // Reading COUNT while its read sync is still pending stalls the bus until the clock returns
  // (a stopped crystal: watchdog reset). So check after every sync, before touching COUNT.
  RTC->MODE0.READREQ.reg = RTC_READREQ_RREQ | RTC_READREQ_ADDR(0x10);
  rtcSync();
  if (rtcStuck) return lastTicks;
  uint32_t low = RTC->MODE0.COUNT.reg;
  uint32_t high = rtcHigh;
  if (RTC->MODE0.INTFLAG.bit.OVF) {
    ++high;
    RTC->MODE0.READREQ.reg = RTC_READREQ_RREQ | RTC_READREQ_ADDR(0x10);
    rtcSync();
    if (rtcStuck) return lastTicks;
    low = RTC->MODE0.COUNT.reg;
  }
  return lastTicks = (uint64_t(high) << 32) | low;
}
uint64_t ticks() {
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  uint64_t t = ticksLocked();
  __set_PRIMASK(mask); return t;
}
// Milliseconds from the RTC: unlike millis(), these keep counting through deep standby.
// Only call while the RTC has a running clock (or rtcStuck, which returns the last value).
uint32_t wallMsLocked() { return uint32_t(ticksLocked() * 1000 / HZ); }
uint32_t wallMs() {
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  uint32_t ms = wallMsLocked();
  __set_PRIMASK(mask); return ms;
}
// Reports are due when a revolution completes or the bucket tips; both counts only grow.
uint64_t reportKey() { return pulses.revolutions + rain.tips; }
void debounceArm() {
  StageScope stage(ST_SCHEDULE);
  uint64_t deadline = nextReport;
  if (txLedUntil && txLedUntil < deadline) deadline = txLedUntil;
  if (SENSOR && !reporting && !reportDue && reports.deadline(reportKey()) < deadline)
    deadline = reports.deadline(reportKey());
  if (pulses.pending && pulses.changedAt + PulseCounter::debounceTicks < deadline)
    deadline = pulses.changedAt + PulseCounter::debounceTicks;
  if (rain.pending && rain.changedAt + TipCounter::debounceTicks < deadline)
    deadline = rain.changedAt + TipCounter::debounceTicks;
  // COMP matches only on COUNT == COMP. If COUNT passes the new value while the write
  // synchronizes, the next match is 2^32 ticks (~36 h) away: verify and re-arm.
  uint64_t margin = 32;
  for (unsigned attempt = 0; ; ++attempt) {
    if (rtcStuck) return; // the clock monitor in loop() re-arms after recovery
    uint64_t now = ticksLocked();
    if (rtcStuck) return;
    if (deadline < now + margin) deadline = now + margin;
    RTC->MODE0.COMP[0].reg = uint32_t(deadline);
    rtcSync();
    if (rtcStuck) return;
    if (ticksLocked() < deadline || attempt == 4) break;
    ++compareRearms; margin *= 2;
  }
}
void reedEdge() {
  StageScope stage(ST_REED_IRQ);
  uint64_t now = ticksLocked();
  ++rawEdges; lastRawChange = now;
  pulses.edge(digitalRead(REED) == HIGH, now);
  debounceArm();
}
void rainEdge() {
  StageScope stage(ST_REED_IRQ);
  uint64_t now = ticksLocked();
  ++rainRawEdges;
  rain.edge(digitalRead(RAIN) == HIGH, now);
  debounceArm();
}
void RTC_Handler() {
  StageScope stage(ST_RTC_IRQ);
  if (RTC->MODE0.INTFLAG.bit.OVF) {
    RTC->MODE0.INTFLAG.reg = RTC_MODE0_INTFLAG_OVF;
    ++rtcHigh;
  }
  if (RTC->MODE0.INTFLAG.bit.CMP0) {
    RTC->MODE0.INTFLAG.reg = RTC_MODE0_INTFLAG_CMP0;
    uint64_t now = ticksLocked();
    lastRtcIrqMs = millis(); lastRtcIrqTicks = now;
    if (txLedUntil && now >= txLedUntil) {
      digitalWrite(LED_BUILTIN, LOW);
      txLedUntil = 0;
    }
    if (SENSOR) {
      pulses.timer(digitalRead(REED) == HIGH, now);
      rain.timer(digitalRead(RAIN) == HIGH, now);
      if (!reporting && reports.due(now, reportKey())) reportDue = true;
    }
    if (now >= nextReport) {
      nextReport = now + 5 * HZ;
      reportDue = true;
    }
    debounceArm();
  }
}
void clockSetup() {
  SYSCTRL->XOSC32K.bit.RUNSTDBY = 1;
  GCLK->GENDIV.reg = GCLK_GENDIV_ID(2);
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(2) | GCLK_GENCTRL_SRC_XOSC32K |
                         GCLK_GENCTRL_GENEN | GCLK_GENCTRL_RUNSTDBY;
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  PM->APBAMASK.reg |= PM_APBAMASK_RTC;
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_RTC | GCLK_CLKCTRL_GEN_GCLK2 | GCLK_CLKCTRL_CLKEN;
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  RTC->MODE0.CTRL.reg = RTC_MODE0_CTRL_SWRST;
  while (RTC->MODE0.CTRL.bit.SWRST || RTC->MODE0.STATUS.bit.SYNCBUSY) {}
  RTC->MODE0.CTRL.reg = RTC_MODE0_CTRL_MODE_COUNT32 | RTC_MODE0_CTRL_PRESCALER_DIV1;
  rtcSync();
  RTC->MODE0.COMP[0].reg = 5 * HZ; rtcSync();
  RTC->MODE0.INTENSET.reg = RTC_MODE0_INTENSET_OVF | RTC_MODE0_INTENSET_CMP0;
  NVIC_SetPriority(RTC_IRQn, 0);
  NVIC_EnableIRQ(RTC_IRQn);
  RTC->MODE0.CTRL.bit.ENABLE = 1; rtcSync();
}
void internalClockBegin() {
  // OSC32K with its factory calibration; feeds GCLK5, unused until a crystal failure.
  uint32_t cal = (*reinterpret_cast<const uint32_t *>(FUSES_OSC32K_CAL_ADDR) & FUSES_OSC32K_CAL_Msk)
                 >> FUSES_OSC32K_CAL_Pos;
  SYSCTRL->OSC32K.reg = SYSCTRL_OSC32K_CALIB(cal) | SYSCTRL_OSC32K_STARTUP(2) |
                        SYSCTRL_OSC32K_EN32K | SYSCTRL_OSC32K_RUNSTDBY | SYSCTRL_OSC32K_ENABLE;
  uint32_t start = millis();
  while (!SYSCTRL->PCLKSR.bit.OSC32KRDY && millis() - start < 100) {}
  GCLK->GENDIV.reg = GCLK_GENDIV_ID(GCLK_INTERNAL);
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(GCLK_INTERNAL) | GCLK_GENCTRL_SRC_OSC32K |
                      GCLK_GENCTRL_GENEN | GCLK_GENCTRL_RUNSTDBY;
  while (GCLK->STATUS.bit.SYNCBUSY) {}
}
bool rtcSyncWithin(uint32_t limit) {
  for (uint32_t n = 0; RTC->MODE0.STATUS.bit.SYNCBUSY; ) if (++n > limit) return false;
  return true;
}
// Interrupts masked. True if the RTC clock domain completes a read sync. Never writes
// an RTC register while a sync is pending (that would stall the bus).
bool rtcClockRunning() {
  if (!rtcSyncWithin(20000)) return false;
  RTC->MODE0.READREQ.reg = RTC_READREQ_RREQ | RTC_READREQ_ADDR(0x10);
  return rtcSyncWithin(20000);
}
// Interrupts masked. Route RTC (and the sensor's reed EIC) to a clock generator. CLKCTRL is
// not write-synchronized; the disable read-back is bounded because a dead clock never confirms.
void routeSlowClock(uint8_t gen) {
  const uint8_t ids[] = {GCLK_CLKCTRL_ID_RTC_Val, GCLK_CLKCTRL_ID_EIC_Val};
  for (uint8_t id : ids) {
    if (id == GCLK_CLKCTRL_ID_EIC_Val && !SENSOR) continue;
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(id);
    for (uint32_t n = 0; n < 20000; ++n) {
      *reinterpret_cast<volatile uint8_t *>(&GCLK->CLKCTRL.reg) = id;
      if (!GCLK->CLKCTRL.bit.CLKEN) break;
    }
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(id) | GCLK_CLKCTRL_GEN(gen) | GCLK_CLKCTRL_CLKEN;
  }
}
void setTicksLocked(uint64_t t) {
  RTC->MODE0.COUNT.reg = uint32_t(t);
  rtcSync();
  rtcHigh = uint32_t(t >> 32);
  RTC->MODE0.INTFLAG.reg = RTC_MODE0_INTFLAG_OVF;
  lastTicks = t;
}
// Interrupts masked, RTC clock running. Advance the RTC by the time it was frozen, measured
// with millis() (CPU clock), so uptime, ages and the period spanning the outage stay sane.
void restoreTimeLocked(uint32_t frozenMs) {
  uint64_t now = ticksLocked();
  uint64_t estimate = lastRtcIrqTicks + uint64_t(frozenMs) * HZ / 1000;
  if (estimate > now) setTicksLocked(estimate);
  lastRtcIrqMs = millis(); lastRtcIrqTicks = ticksLocked();
  debounceArm();
}
void clockMonitor() {
  uint32_t nowMs = millis();
  if (xoscTestUntil && !rtcStuck && int32_t(wallMs() - xoscTestUntil) >= 0) {
    xoscTestUntil = 0;
    SYSCTRL->XOSC32K.bit.ENABLE = 1;
    logLine("# xosc-test: crystal oscillator re-enabled");
  }
  noInterrupts();
  // millis() only advances while awake, so in deep standby a dead RTC shows up as the WDT
  // early warning instead: nothing kicked the WDT for 8 s, i.e. no RTC or reed wake-up.
  uint32_t quiet = nowMs - lastRtcIrqMs;
  if (wdtEarly) {
    wdtEarly = false;
    uint32_t q = WDT_EARLY_MS + (nowMs - wdtEarlyMs);
    if (q > quiet) quiet = q;
  }
  bool failed = rtcStuck || quiet >= RTC_QUIET_MS;
  // On the internal clock the RTC runs, so its wall time paces the retries (also in standby).
  bool retry = !failed && onInternalClock && wallMsLocked() - lastCrystalTryMs >= CRYSTAL_RETRY_MS;
  if (failed && !rtcStuck && rtcClockRunning()) {
    // Clock is fine; a compare match was missed. Re-arm without changing source.
    ++rtcStallRearms; lastRtcIrqMs = nowMs; debounceArm();
    interrupts(); return;
  }
  if (failed) {
    // Switch to the other source; a pending sync completes on the new clock.
    bool toInternal = !onInternalClock;
    routeSlowClock(toInternal ? GCLK_INTERNAL : GCLK_CRYSTAL);
    if (rtcSyncWithin(20000)) {
      rtcStuck = false;
      onInternalClock = toInternal;
      if (toInternal) ++clockFailovers;
      clockLostMs += quiet;
      restoreTimeLocked(quiet);
      lastCrystalTryMs = wallMsLocked();
    }
    interrupts();
    logLine(rtcStuck ? "# RTC clock failed; no working 32 kHz source yet"
                     : toInternal ? "# 32 kHz crystal stopped; RTC/EIC now on internal OSC32K"
                                  : "# internal clock failed; RTC/EIC back on crystal");
    return;
  }
  if (retry) {
    lastCrystalTryMs = wallMsLocked();
    uint64_t before = ticksLocked();
    uint32_t beforeMs = millis();
    routeSlowClock(GCLK_CRYSTAL);
    bool ok = rtcClockRunning();
    if (!ok) {
      routeSlowClock(GCLK_INTERNAL);
      if (!rtcSyncWithin(20000)) rtcStuck = true;
    }
    if (!rtcStuck) {
      onInternalClock = !ok;
      if (ok) ++clockRestores;
      lastRtcIrqTicks = before;
      restoreTimeLocked(millis() - beforeMs);
    }
    interrupts();
    if (ok) logLine("# 32 kHz crystal running again; RTC/EIC restored to crystal");
    return;
  }
  interrupts();
}
void wdtBegin() {
  // GCLK4 = OSCULP32K / 32 = 1024 Hz; 16384 cycles = 16 s. Resets on a genuine stall;
  // pulse counts are RAM-only and restart (new session), but the trace is retained.
  GCLK->GENDIV.reg = GCLK_GENDIV_ID(4) | GCLK_GENDIV_DIV(4);
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(4) | GCLK_GENCTRL_SRC_OSCULP32K | GCLK_GENCTRL_GENEN |
                      GCLK_GENCTRL_DIVSEL | GCLK_GENCTRL_RUNSTDBY;
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_WDT | GCLK_CLKCTRL_GEN_GCLK4 | GCLK_CLKCTRL_CLKEN;
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  WDT->CTRL.reg = 0; while (WDT->STATUS.bit.SYNCBUSY) {}
  WDT->CONFIG.reg = WDT_CONFIG_PER_16K;
  WDT->EWCTRL.reg = WDT_EWCTRL_EWOFFSET_8K; // early warning 8 s after a kick (see WDT_EARLY_MS)
  WDT->INTFLAG.reg = WDT_INTFLAG_EW;
  WDT->INTENSET.reg = WDT_INTENSET_EW;
  NVIC_SetPriority(WDT_IRQn, 1);
  NVIC_EnableIRQ(WDT_IRQn);
  WDT->CTRL.reg = WDT_CTRL_ENABLE; while (WDT->STATUS.bit.SYNCBUSY) {}
}
void WDT_Handler() {
  // Healthy loops kick every few seconds at most; this fires only if nothing woke the CPU.
  WDT->INTFLAG.reg = WDT_INTFLAG_EW;
  wdtEarly = true; wdtEarlyMs = millis(); ++wdtEarlyWakes;
}
void wdtKick() {
  // Writing CLEAR during synchronization stalls the bus; skip, the next kick will land.
  if (!WDT->STATUS.bit.SYNCBUSY) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
}
void logLine(const char *s) {
  StageScope stage(ST_USB_LOG);
  // USB writes have a bounded core timeout; counting remains interrupt driven.
  if (usbLive && Serial) Serial.println(s);
}
bool getRadioLine() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\r') continue;
    if (c == '\n') {
      radioLine[radioLen] = 0; radioLen = 0;
      if (*radioLine) return true;
    } else if (radioLen < sizeof(radioLine)-1) radioLine[radioLen++] = c;
    else radioLen = 0;
  }
  return false;
}
bool radioWriteLine(const char *s) {
  if (radioUart.line(s)) return true;
  radioLen = 0; radioOK = false;
  logLine("# UART transmit timeout; attempt failed, bounded UART recovery attempted");
  return false;
}
bool command(const char *s, const char *expected = "+OK", uint32_t timeout = 1500) {
  StageScope stage(ST_RADIO_WRITE);
  traceCommand(s);
  while (getRadioLine()) logLine(radioLine);
  if (!radioWriteLine(s)) return false;
  traceStage(ST_RADIO_WAIT);
  uint32_t start = millis();
  while (millis() - start < timeout) {
    if (getRadioLine()) {
      logLine(radioLine);
      if (!strcmp(radioLine, expected)) return true;
      if (!strncmp(radioLine, "+ERR", 4)) return false;
    }
    __WFI();
  }
  logLine("# radio command failed/timed out");
  return false;
}
bool wakeRadio() {
  StageScope stage(ST_WAKE_WRITE);
  traceCommand("AT (wake)");
  // First UART input wakes MODE=1; do not query MODE after putting it to sleep.
  if (!radioWriteLine("AT")) return false;
  traceStage(ST_WAKE_DELAY); delay(30);
  traceStage(ST_WAKE_DRAIN);
  while (getRadioLine()) {}
  return command("AT");
}
bool configureRadio() {
  StageScope stage(ST_RADIO_CONFIG);
  if (!wakeRadio()) {
    const uint32_t bauds[] = {9600, 57600, 38400, 19200, 4800, 2400, 1200, 115200};
    bool found = false;
    for (uint32_t baud : bauds) {
      wdtKick();
      if (!radioUart.begin(baud)) return false;
      radioLen = 0;
      char msg[50]; snprintf(msg,sizeof(msg),"# probing radio baud %lu",(unsigned long)baud); logLine(msg);
      if (wakeRadio()) { found = true; break; }
    }
    if (!found) return false;
    command("AT+IPR=115200", "+IPR=115200");
    delay(100);
    if (!radioUart.begin(115200)) return false;
    if (!wakeRadio()) return false;
  }
  if (!command("AT+MODE=0")) return false;
  if (!command(SENSOR ? "AT+ADDRESS=1" : "AT+ADDRESS=2")) return false;
  if (!command("AT+NETWORKID=18")) return false;
  if (!command("AT+BAND=915000000")) return false;
  if (!command("AT+PARAMETER=7,9,1,12")) return false;
  if (!command("AT+CRFOP=2")) return false;
  return command("AT+BAND?", "+BAND=915000000") &&
         command("AT+PARAMETER?", "+PARAMETER=7,9,1,12") &&
         command("AT+CRFOP?", "+CRFOP=2");
}
void radioHardReset() {
  StageScope stage(ST_RADIO_CONFIG);
  traceCommand("NRST");
  ++radioResets;
  logLine("# repeated radio failures; pulsing RYLR998 NRST (D6)");
  digitalWrite(RADIO_RESET, LOW); delay(100); digitalWrite(RADIO_RESET, HIGH);
  // Module prints +READY after boot; settings are re-sent by configureRadio().
  uint32_t start = millis();
  while (millis() - start < 1500) {
    if (getRadioLine()) { logLine(radioLine); if (!strcmp(radioLine, "+READY")) break; }
    __WFI();
  }
  radioOK = false;
}
void decimal64(uint64_t n, char *out) {
  char temp[21]; unsigned i = 0;
  do { temp[i++] = '0' + n % 10; n /= 10; } while(n);
  unsigned j = 0; while(i) out[j++] = temp[--i]; out[j] = 0;
}
// Appends ",<lowercase hex>" (1-16 digits); newlib-nano printf has no %llx.
char *hex64(uint64_t n, char *out) {
  char temp[16]; unsigned i = 0;
  do { temp[i++] = "0123456789abcdef"[n & 15]; n >>= 4; } while (n);
  *out++ = ',';
  while (i) *out++ = temp[--i];
  return out;
}
// Battery millivolts: average of 8 12-bit reads, 3.3 V full scale, x2 for the divider.
// On USB the charger holds this near 4.2 V; it reflects the cell only on battery.
// ---- BMP180 barometer on I2C (SDA/SCL, 0x77) ----
// Read at most every BARO_EVERY_MS, inside report(): ~31 ms of delay() while counting continues
// in interrupts. Absent or failing: fields are sent as 0 and probing is retried every 10 min.
// A watchdog reset during an I2C access (stuck bus) disables the barometer until the next boot.
constexpr uint8_t BMP180_ADDR = 0x77, BMP_OSS = 3; // ultra high resolution: 25.5 ms, ~0.03 hPa rms
constexpr uint32_t BARO_EVERY_MS = 30000, BARO_RETRY_MS = 600000, BARO_STALE_MS = 300000;
Bmp180Cal baroCal;
bool baroOK = false, baroDisabled = false, baroHave = false;
int32_t baroDeciC = 0, baroPa = 0;
uint32_t baroReadMs = 0, baroProbeMs = 0, baroGoodMs = 0, baroErrors = 0;
bool i2cRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(BMP180_ADDR); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(BMP180_ADDR, n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) buf[i] = Wire.read();
  return true;
}
bool i2cWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(BMP180_ADDR); Wire.write(reg); Wire.write(v);
  return Wire.endTransmission() == 0;
}
bool baroBegin() {
  StageScope stage(ST_BARO);
  baroProbeMs = wallMs();
  uint8_t id = 0, e[22];
  if (!i2cRead(0xD0, &id, 1) || id != 0x55 || !i2cRead(0xAA, e, 22)) return false;
  auto w = [&](int i) { return uint16_t(e[2*i] << 8 | e[2*i+1]); };
  baroCal = {int16_t(w(0)), int16_t(w(1)), int16_t(w(2)), w(3), w(4), w(5),
             int16_t(w(6)), int16_t(w(7)), int16_t(w(8)), int16_t(w(9)), int16_t(w(10))};
  return baroCal.plausible();
}
bool baroRead() {
  StageScope stage(ST_BARO);
  uint8_t b[3];
  if (!i2cWrite(0xF4, 0x2E)) return false;
  delay(5);
  if (!i2cRead(0xF6, b, 2)) return false;
  const int32_t ut = int32_t(b[0]) << 8 | b[1];
  if (!i2cWrite(0xF4, 0x34 | (BMP_OSS << 6))) return false;
  delay(26);
  if (!i2cRead(0xF6, b, 3)) return false;
  const int32_t up = (int32_t(b[0]) << 16 | int32_t(b[1]) << 8 | b[2]) >> (8 - BMP_OSS);
  int32_t t, pa;
  bmp180Compensate(baroCal, ut, up, BMP_OSS, t, pa);
  if (pa < 30000 || pa > 110000 || t < -500 || t > 850) return false;
  baroDeciC = t; baroPa = pa;
  return true;
}
void baroService() {
  if (baroDisabled) return;
  const uint32_t ms = wallMs(); // RTC time: millis() stands still in deep standby
  if (!baroOK) {
    if (baroProbeMs && ms - baroProbeMs < BARO_RETRY_MS) return;
    if (!(baroOK = baroBegin())) return;
    baroReadMs = ms - BARO_EVERY_MS;
  }
  if (baroHave && ms - baroReadMs < BARO_EVERY_MS) return;
  baroReadMs = ms;
  if (baroRead()) { baroHave = true; baroGoodMs = ms; }
  else if (++baroErrors, ms - baroGoodMs > BARO_STALE_MS) baroOK = false; // re-probe later
}
void i2cScan() {
  StageScope stage(ST_BARO);
  char msg[200] = "# i2c devices:"; size_t n = strlen(msg);
  for (uint8_t a = 1; a < 127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0 && n + 6 < sizeof(msg)) n += snprintf(msg + n, sizeof(msg) - n, " 0x%02x", a);
    wdtKick();
  }
  if (n == strlen("# i2c devices:")) strcat(msg, " none");
  logLine(msg);
}
uint32_t readVbatMv() {
  uint32_t sum = 0;
  for (unsigned i = 0; i < 8; ++i) sum += analogRead(VBAT);
  return (sum * 2 * 3300 + 8 * 4095 / 2) / (8 * 4095);
}
void report() {
  StageScope stage(ST_REPORT);
  traceStage(ST_SNAPSHOT);
  uint64_t count, revolutions, intervalA, intervalB, age, revAge, now, edges, pinAge;
  uint64_t tips, tipInterval, tipAge, tipEdges, key;
  bool pin, rainPin;
  noInterrupts();
  now = ticksLocked(); count = pulses.count; revolutions = pulses.revolutions;
  intervalA = pulses.intervalA; intervalB = pulses.intervalB;
  age = pulses.havePulse ? now - pulses.last : UINT64_MAX;
  revAge = revolutions ? now - pulses.lastRevolution : UINT64_MAX;
  pin = digitalRead(REED) == HIGH; edges = rawEdges;
  pinAge = edges ? now - lastRawChange : UINT64_MAX;
  tips = rain.tips; tipInterval = rain.interval; tipAge = rain.haveTip ? now - rain.last : UINT64_MAX;
  rainPin = digitalRead(RAIN) == HIGH; tipEdges = rainRawEdges;
  key = revolutions + tips;
  bool due = forceReport || reports.due(now, key);
  forceReport = false;
  interrupts();
  traceStage(ST_REPORT);
  if (!due) return;
  const uint32_t vbatMv = readVbatMv();
  baroService();
  const bool baroFresh = baroHave && wallMs() - baroGoodMs < BARO_STALE_MS;
  const uint64_t pressurePa = baroFresh ? baroPa : 0, tempDeciK = baroFresh ? baroDeciC + 2732 : 0; // 0 = none
  char payload[256], cmd[288];
  // W8 = W7 + sleep mode digit (0 = light sleep, 1 = deep standby).
  // W7: every number is lowercase hex. uint64 fields take <= 16 digits; A, B and rain_interval
  // are capped at ffffffff (36 h), pressure (<= 110000 Pa) at 5 and temperature at 3, so the worst
  // case is provably 221 bytes, under the radio's 240-byte limit (and the receiver's
  // 255-character line). Revolutions and period are omitted: the receiver derives them exactly
  // (revolutions = (count-1)/2, period = A + B).
  //   W7,session,seq,count,A,B,age,rev_age,uptime,pin,pin_age,raw_edges,clock,
  //      rain_tips,rain_interval,rain_age,rain_pin,rain_raw_edges,vbat_mv,pressure_pa,temp_dk
  // temp_dk = 0.1 K (deg C x 10 + 2732); pressure/temp 0 = no barometer reading.
  // Pins: 1 = open/high. rain_interval 0 = fewer than two tips; ages all-f = never.
  auto cap32 = [](uint64_t v) { return v > 0xffffffffULL ? uint64_t(0xffffffffULL) : v; };
  const uint64_t fields[] = {count,cap32(intervalA),cap32(intervalB),age,revAge,now};
  char *out = payload + snprintf(payload,sizeof(payload),"W8,%08lx,%lx",
                                 (unsigned long)session,(unsigned long)++sequence);
  for (uint64_t v : fields) out = hex64(v,out);
  *out++ = ','; *out++ = '0' + pin;
  out = hex64(pinAge,out); out = hex64(edges,out);
  *out++ = ','; *out++ = '0' + onInternalClock;
  out = hex64(tips,out); out = hex64(cap32(tipInterval),out); out = hex64(tipAge,out);
  *out++ = ','; *out++ = '0' + rainPin;
  out = hex64(tipEdges,out);
  out = hex64(vbatMv,out);
  out = hex64(pressurePa,out); out = hex64(tempDeciK,out);
  *out++ = ','; *out++ = '0' + deepSleep;
  *out = 0;
  logLine(payload);
  if (SENSOR && consecutiveFails >= RADIO_RESET_AFTER_FAILS &&
      consecutiveFails % RADIO_RESET_AFTER_FAILS == 0) radioHardReset();
  if (!radioOK) radioOK = configureRadio();
  wdtKick();
  bool sent = false;
  if (radioOK && wakeRadio()) {
    snprintf(cmd,sizeof(cmd),"AT+SEND=2,%u,%s",(unsigned)strlen(payload),payload);
    sent = command(cmd, "+OK", 2500);
    if (sent) {
      noInterrupts();
      digitalWrite(LED_BUILTIN, HIGH);
      txLedUntil = ticksLocked() + 164; // 5.005 ms, timer-driven; never blocks counting
      debounceArm();
      interrupts();
    }
    // SEND's +OK means the module has finished transmitting.
    if (!command("AT+MODE=1")) radioOK = false;
  } else radioOK = false;
  if (sent) { ++txOK; consecutiveFails = 0; } else { ++txFailed; ++consecutiveFails; }
  noInterrupts();
  reports.finished(ticksLocked(), key, sent);
  interrupts();
}
void sleepSensor() {
  StageScope stage(ST_IDLE);
  if (!deepSleep) {
    // Light sleep: SysTick wakes every 1 ms, USB keeps working.
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
    __DSB(); __WFI();
    return;
  }
  // Deep standby. Interrupts are masked so a report flagged just before WFI is not slept
  // through (WFI still wakes on a pending interrupt; the handler runs after unmasking).
  __disable_irq();
  if (reportDue || forceReport) { __enable_irq(); return; }
  ++standbyEntries;
  SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk; // as ArduinoLowPower: a pending SysTick must not wake/hang
  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
  __DSB(); __WFI();
  SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk; // the other __WFI() calls (radio waits) rely on light sleep
  SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;
  ++standbyReturns;
  __enable_irq();
}
bool usbHostActive() {
  // USB start-of-frame counter advances only while a host is talking to us.
  uint16_t frame = USB->DEVICE.FNUM.bit.FNUM;
  delay(3);
  return frame != USB->DEVICE.FNUM.bit.FNUM;
}
// Once a USB host is seen, stay in light sleep for the session (USB and !status keep
// working). Plugging USB in later: press reset once, or wait for the next report to notice.
void updateSleepMode() {
  if (usbLive && !usbDetachedForTest) usbSeen = true;
  deepSleep = SENSOR && !usbSeen;
}
void setup() {
  resetCause = PM->RCAUSE.reg;
  traceBegin(resetCause);
  StageScope stage(ST_SETUP);
  // SAMD21 standby erratum workaround, also used in ArduinoLowPower configGCLK6().
  // Do not allow flash to power completely down during standby wake transitions.
  NVMCTRL->CTRLB.bit.SLEEPPRM = NVMCTRL_CTRLB_SLEEPPRM_DISABLED_Val;
  pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, LOW);
  pinMode(8, OUTPUT); digitalWrite(8, LOW); // Adalogger green SD LED
  pinMode(4, OUTPUT); digitalWrite(4, HIGH); // deselect SD; no card needed
  pinMode(REED, INPUT_PULLUP);
  pinMode(RAIN, INPUT_PULLUP);
  pinMode(RADIO_RESET, OUTPUT); digitalWrite(RADIO_RESET, HIGH);
  Serial.begin(115200); radioUart.begin(115200);
  clockSetup();
  internalClockBegin();
  lastRtcIrqMs = millis();
  if (SENSOR) {
    pulses.begin(digitalRead(REED) == HIGH);
    attachInterrupt(digitalPinToInterrupt(REED), reedEdge, CHANGE);
    rain.begin(digitalRead(RAIN) == HIGH);
    attachInterrupt(digitalPinToInterrupt(RAIN), rainEdge, CHANGE);
    // Reed edges wake the CPU from standby.
    EIC->WAKEUP.reg |= (1u << g_APinDescription[REED].ulExtInt) | (1u << g_APinDescription[RAIN].ulExtInt);
    NVIC_SetPriority(EIC_IRQn, 0);
    // CHANGE detection needs a running EIC clock even in standby.
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_EIC | GCLK_CLKCTRL_GEN_GCLK2 | GCLK_CLKCTRL_CLKEN;
    while (GCLK->STATUS.bit.SYNCBUSY) {}
  }
  for (unsigned i=0;i<32;i++) session = (session << 1) ^ analogRead(A0) ^ uint32_t(ticks());
  analogReadResolution(12); // battery reads
  if (SENSOR) {
    Wire.begin(); Wire.setClock(100000);
    // A watchdog reset inside an I2C access means a stuck bus: leave the barometer off this boot.
    baroDisabled = previousTraceValid && previousTrace.stage == ST_BARO;
    if (!baroDisabled) baroOK = baroBegin();
  }
  traceRecord.session = session;
  wdtBegin();
  delay(1500);
  usbLive = usbHostActive();
  updateSleepMode();
  logLine(SENSOR ? "# SENSOR wind D5, rain D10 (each closure = 1 tip); 32768 ticks/s; debounce=328 ticks" : "# RECEIVER address2; 915MHz SF7 BW500 CR4/5 2dBm");
  radioOK = configureRadio();
  if (SENSOR && radioOK) command("AT+MODE=1");
  noInterrupts();
  reporting = false;
  reportDue = true;
  lastRtcIrqMs = millis();
  debounceArm();
  interrupts();
}
void serviceUsb() {
  StageScope stage(ST_USB_SERVICE);
  static char input[32]; static unsigned used = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      input[used] = 0; used = 0;
      if (!strcmp(input,"!report") && SENSOR) {
        forceReport = true; reportDue = true;
      } else if (!strcmp(input,"!standby-test") && SENSOR) {
        logLine(deepSleep ? "# deep standby is active" : "# light sleep: a USB host was seen this session; unplug USB and reset to use deep standby");
      } else if (!strcmp(input,"!uart-test") && SENSOR) {
        // Simulate a missing TX-complete indication on the next wake command.
        radioUart.injectCompletionTimeout = true;
        forceReport = true; reportDue = true;
        logLine("# injecting one UART completion timeout; automatic retry expected in 5 s");
      } else if (!strcmp(input,"!xosc-test") || (!strcmp(input,"!xosc-test deep") && SENSOR)) {
        // Simulate a stopped crystal for 90 s (also removes the DFLL reference, as a real stop
        // does). "deep" forces deep standby and detaches USB until the crystal is restored
        // (the next crystal retry, up to ~10 min after failover); read !status then.
        testForcesDeep = input[10] == ' ';
        if (testForcesDeep) {
          logLine("# xosc-test deep: USB detaching; crystal off 90 s in deep standby; USB returns after the crystal is restored (up to ~11 min)");
          Serial.flush(); delay(50);
          USBDevice.detach(); usbLive = false; usbDetachedForTest = true;
          usbSeen = false; updateSleepMode();
        } else logLine("# xosc-test: crystal oscillator disabled for 90 s; expect failover within ~8 s");
        xoscTestUntil = wallMs() + 90000; // while the RTC still runs
        SYSCTRL->XOSC32K.bit.ENABLE = 0;
      } else if (!strcmp(input,"!clock-retry")) {
        lastCrystalTryMs = wallMs() - CRYSTAL_RETRY_MS;
        logLine("# crystal retry requested");
      } else if (!strcmp(input,"!diag-reset")) {
        // Explicit diagnostic retention test only; never invoked automatically.
        traceStage(ST_MANUAL_RESET);
        NVIC_SystemReset();
      } else if (!strcmp(input,"!status")) {
        char c[21], r[21], p[21], msg[160];
        noInterrupts();
        uint64_t n = pulses.count, rev = pulses.revolutions, t = pulses.revolutionPeriod, e = rawEdges;
        interrupts();
        char re[21];
        decimal64(n,c); decimal64(rev,r); decimal64(t,p); decimal64(e,re);
        snprintf(msg,sizeof(msg),"# %s W8 radio=%s pin5=%d pulses=%s revolutions=%s rev_period_ticks=%s raw_edges=%s",
                 SENSOR ? "sensor" : "receiver",radioOK ? "OK" : "ERROR",digitalRead(REED),c,r,p,re);
        logLine(msg);
        noInterrupts();
        uint64_t tipsNow = rain.tips, tipEdgesNow = rainRawEdges;
        interrupts();
        decimal64(tipsNow,c); decimal64(tipEdgesNow,re);
        snprintf(msg,sizeof(msg),"# rain D10=%d tips=%s raw_edges=%s (1 tip per debounced closure; clean = 2 edges per tip) vbat=%lu mV",
                 digitalRead(RAIN),c,re,(unsigned long)readVbatMv());
        logLine(msg);
        snprintf(msg,sizeof(msg),"# baro BMP180 %s pressure=%ld Pa temp=%s%ld.%ld C errors=%lu last_good_ms_ago=%lu",
                 baroDisabled ? "DISABLED(wdt during i2c)" : baroOK ? "OK" : "absent",(long)baroPa,baroDeciC < 0 ? "-" : "",
                 (long)(abs(baroDeciC)/10),(long)(abs(baroDeciC)%10),(unsigned long)baroErrors,(unsigned long)(baroHave ? wallMs()-baroGoodMs : 0));
        logLine(msg);
        snprintf(msg,sizeof(msg),"# fw=" FIRMWARE " session=%08lx reset=0x%02x sleepprm=%u standby=%lu/%lu tx_ok=%lu tx_fail=%lu",
                 (unsigned long)session,resetCause,(unsigned)NVMCTRL->CTRLB.bit.SLEEPPRM,
                 (unsigned long)standbyEntries,(unsigned long)standbyReturns,
                 (unsigned long)txOK,(unsigned long)txFailed);
        logLine(msg);
        char diag[256], cmd[72];
        for (unsigned i=0; i<sizeof(cmd); ++i) cmd[i] = traceRecord.command[i];
        cmd[sizeof(cmd)-1] = 0;
        snprintf(diag,sizeof(diag),"# sleep=%s wdt_early=%lu stage=%s parent=%s since_ms=%lu last_command=%s",
                 deepSleep ? "deep" : "light",(unsigned long)wdtEarlyWakes,stageName(traceRecord.stage),stageName(traceRecord.parent),(unsigned long)traceRecord.sinceMs,cmd);
        logLine(diag);
        snprintf(diag,sizeof(diag),"# uart_ready=%u baud=%lu timeouts=%lu recoveries=%lu last_fault=%s injected=%u flags=0x%02lx status=0x%04lx ctrla=0x%08lx sync=0x%08lx",
                 radioUart.ready,(unsigned long)radioUart.baud,(unsigned long)radioUart.timeouts,
                 (unsigned long)radioUart.recoveries,radioUart.lastFault,radioUart.lastInjected,
                 (unsigned long)radioUart.lastFlags,(unsigned long)radioUart.lastStatus,
                 (unsigned long)radioUart.lastCtrlA,(unsigned long)radioUart.lastSync);
        logLine(diag);
        snprintf(diag,sizeof(diag),"# txc_inferred=%lu rtc_sync_timeouts=%lu compare_rearms=%lu radio_resets=%lu consecutive_fails=%lu wdt=16s",
                 (unsigned long)radioUart.inferredCompletions,(unsigned long)rtcSyncTimeouts,
                 (unsigned long)compareRearms,(unsigned long)radioResets,(unsigned long)consecutiveFails);
        logLine(diag);
        snprintf(diag,sizeof(diag),"# clock=%s rtc_stuck=%u failovers=%lu restores=%lu stall_rearms=%lu lost_ms=%lu since_rtc_irq_ms=%lu",
                 onInternalClock ? "internal" : "crystal",(unsigned)rtcStuck,(unsigned long)clockFailovers,
                 (unsigned long)clockRestores,(unsigned long)rtcStallRearms,(unsigned long)clockLostMs,
                 (unsigned long)(millis() - lastRtcIrqMs));
        logLine(diag);
        if (previousTraceValid) {
          snprintf(diag,sizeof(diag),"# previous_trace session=%08lx stage=%s parent=%s since_ms=%lu command=%s",
                   (unsigned long)previousTrace.session,stageName(previousTrace.stage),stageName(previousTrace.parent),
                   (unsigned long)previousTrace.sinceMs,previousTrace.command);
          logLine(diag);
        } else logLine("# previous_trace unavailable (power loss, overwrite, or first diagnostic boot)");
      } else if (!strcmp(input,"!i2c-scan") && SENSOR) {
        if (baroDisabled) logLine("# i2c disabled this boot (watchdog reset during I2C)");
        else i2cScan();
      } else if (!strcmp(input,"!led on")) digitalWrite(LED_BUILTIN,HIGH);
      else if (!strcmp(input,"!led off")) digitalWrite(LED_BUILTIN,LOW);
    } else if (used < sizeof(input)-1) input[used++] = ch;
    else used = 0;
  }
}
void loop() {
  StageScope stage(ST_LOOP);
  wdtKick();
  serviceUsb();
  clockMonitor();
  if (usbDetachedForTest && !xoscTestUntil && !onInternalClock && !rtcStuck) {
    usbDetachedForTest = false;
    if (testForcesDeep) usbSeen = false; // stay deep until a report sees the host again
    USBDevice.attach();
  }
  if (SENSOR) {
    bool due;
    noInterrupts();
    due = reportDue;
    if (due) { reporting = true; reportDue = false; }
    interrupts();
    if (due) {
      usbLive = usbHostActive();
      updateSleepMode();
      report();
      noInterrupts();
      reporting = false;
      // Includes pulses accepted while the radio was busy; never clear their count.
      debounceArm();
      interrupts();
    }
    sleepSensor();
  } else {
    while (getRadioLine()) logLine(radioLine);
    if (reportDue) {
      reportDue = false;
      if (!radioOK) radioOK = configureRadio();
      logLine(radioOK ? "# receiver listening" : "# receiver radio unavailable");
    }
    __WFI();
  }
}
