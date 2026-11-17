#pragma once
// Sensor services for the sketch: BMP180 barometer and SHT31 air temperature/humidity on the
// software I2C bus, bus health checks, and the I2C diagnostics behind !i2c-scan and !i2c-probe.
// Part of link.ino (included there, after the bus object `bb`): it uses the sketch's wallMs(),
// wdtKick(), logLine() and StageScope.
// ---- BMP180 barometer on I2C (SDA/SCL, 0x77) ----
// Read at most every BARO_EVERY_MS, inside report(): ~31 ms of delay() while counting continues
// in interrupts. Absent or failing: fields are sent as 0 and probing is retried every minute.
// A reset during an I2C access (stuck bus) turns the bus off for I2C_RESET_RETRY_MS (i2cReady()).
constexpr uint8_t BMP180_ADDR = 0x77, BMP_OSS = 3; // ultra high resolution: 25.5 ms, ~0.03 hPa rms
constexpr uint32_t BARO_EVERY_MS = 30000, BARO_RETRY_MS = 60000, BARO_STALE_MS = 300000;
Bmp180Cal baroCal;
bool baroOK = false, i2cOff = false, baroHave = false;
// Why I2C is off this boot (i2cOff): set in setup().
const char *i2cOffReason = "";
// I2C off is retried by i2cReady(): after a minute when lines read low, after 10 minutes when the
// last boot ended in a reset during an I2C access (a slower retry, so a bus that hangs the
// software master despite its timeouts costs one reset per 10 minutes, not one per minute).
constexpr uint32_t I2C_LINES_RETRY_MS = 60000, I2C_RESET_RETRY_MS = 600000;
uint32_t i2cRetryMs = I2C_LINES_RETRY_MS;
// Idle I2C lines must both read high (the breakouts' pull-ups). Checked at boot, with the
// internal pull-ups as a backstop: a low line means an unpowered sensor (its ESD diodes clamp the
// bus), a short or a slave stuck mid-byte.
// A slave holding SDA gets up to 9 SCL pulses to finish its byte. Returns 0 or a reason.
const char *i2cLinesCheck() {
  // Continuous input sampling for SDA and SCL. By default the SAMD21 samples a pin on demand,
  // and a read right after its input is enabled can return a stale 0: at boot this reported
  // "SDA and SCL held low" on a healthy bus (Nov 16).
  const PinDescription &da = g_APinDescription[PIN_WIRE_SDA], &cl = g_APinDescription[PIN_WIRE_SCL];
  PORT->Group[da.ulPort].CTRL.reg |= 1ul << da.ulPin;
  PORT->Group[cl.ulPort].CTRL.reg |= 1ul << cl.ulPin;
  pinMode(PIN_WIRE_SDA, INPUT_PULLUP); pinMode(PIN_WIRE_SCL, INPUT_PULLUP);
  delayMicroseconds(100);
  for (int i = 0; i < 9 && digitalRead(PIN_WIRE_SCL) && !digitalRead(PIN_WIRE_SDA); ++i) {
    pinMode(PIN_WIRE_SCL, OUTPUT); digitalWrite(PIN_WIRE_SCL, LOW); delayMicroseconds(10);
    pinMode(PIN_WIRE_SCL, INPUT_PULLUP); delayMicroseconds(10);
  }
  const bool sda = digitalRead(PIN_WIRE_SDA), scl = digitalRead(PIN_WIRE_SCL);
  if (!sda && !scl) return "SDA and SCL held low";
  if (!sda) return "SDA held low";
  if (!scl) return "SCL held low";
  return nullptr;
}
// True when the sensor bus may be used. While off, the lines are re-checked every i2cRetryMs, so a
// bus that recovers comes back without a reset.
uint32_t i2cLinesCheckedMs = 0;
bool i2cReady(bool force = false) {
  if (!i2cOff) return true;
  const uint32_t ms = wallMs();
  if (!force && ms - i2cLinesCheckedMs < i2cRetryMs) return false;
  i2cLinesCheckedMs = ms;
  i2cRetryMs = I2C_LINES_RETRY_MS;
  if (const char *lines = i2cLinesCheck()) { i2cOffReason = lines; return false; }
  i2cOff = false; i2cOffReason = "";
  return true;
}
int32_t baroDeciC = 0, baroPa = 0;
uint32_t baroReadMs = 0, baroProbeMs = 0, baroGoodMs = 0, baroErrors = 0;
bool i2cRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  return bb.write(BMP180_ADDR, &reg, 1, false) && bb.read(BMP180_ADDR, buf, n);
}
bool i2cWrite(uint8_t reg, uint8_t v) {
  const uint8_t d[2] = {reg, v};
  return bb.write(BMP180_ADDR, d, 2);
}
bool baroBegin() {
  StageScope stage(ST_I2C);
  bb.clear();
  baroProbeMs = wallMs();
  uint8_t id = 0, e[22];
  if (!i2cRead(0xD0, &id, 1) || id != 0x55 || !i2cRead(0xAA, e, 22)) return false;
  auto w = [&](int i) { return uint16_t(e[2*i] << 8 | e[2*i+1]); };
  baroCal = {int16_t(w(0)), int16_t(w(1)), int16_t(w(2)), w(3), w(4), w(5),
             int16_t(w(6)), int16_t(w(7)), int16_t(w(8)), int16_t(w(9)), int16_t(w(10))};
  return baroCal.plausible();
}
bool baroRead() {
  StageScope stage(ST_I2C);
  bb.clear();
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
  if (!i2cReady()) return;
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
// ---- SHT31 air temperature and humidity on the same bus (0x44, or 0x45 with ADDR high) ----
// Same cadence and failure handling as the barometer: single-shot reads every SHT_EVERY_MS
// (~16 ms of delay()), fields 0 when absent or stale, re-probed every minute (also after 3 failed
// reads in a row), and off for 10 minutes after a reset during any I2C access (the stage is
// shared: ST_I2C).
constexpr uint32_t SHT_EVERY_MS = 30000;
constexpr uint8_t SHT_FAILS_TO_PROBE = 3;
uint8_t shtAddr = 0;  // 0 = not found
bool shtHave = false;
uint16_t shtDeciK = 0, shtPermille = 0;
uint32_t shtReadMs = 0, shtProbeMs = 0, shtGoodMs = 0, shtErrors = 0;
uint8_t shtFailsInRow = 0;
bool shtCommand(uint8_t addr, uint16_t cmd) {
  const uint8_t d[2] = {uint8_t(cmd >> 8), uint8_t(cmd)};
  return bb.write(addr, d, 2);
}
char shtProbeLog[120] = "never probed";
// Status register (0xF32D) with its CRC checked: proves two-way communication. Logs into shtProbeLog.
bool shtStatus(uint8_t addr, size_t &n, const char *tag) {
  uint8_t b[3] = {0, 0, 0};
  const bool cmd = shtCommand(addr, 0xF32D);
  const char *w1 = bb.fault ? bb.why : "";
  bool rd = false;
  if (cmd) { delayMicroseconds(500); rd = bb.read(addr, b, 3); }
  const bool crc = rd && sht31Crc(b, 2) == b[2];
  n += snprintf(shtProbeLog + n, sizeof(shtProbeLog) - n, "0x%02x%s cmd=%d%s read=%d %02x%02x/%02x crc=%d%s; ",
                addr, tag, cmd, w1, rd, b[0], b[1], b[2], crc, rd || !cmd ? "" : bb.why);
  if (n >= sizeof(shtProbeLog)) n = sizeof(shtProbeLog) - 1;
  return crc;
}
bool shtBegin() {
  StageScope stage(ST_I2C);
  shtProbeMs = wallMs();
  shtFailsInRow = 0;
  static const uint8_t addrs[] = {0x44, 0x45};
  size_t n = 0; shtProbeLog[0] = 0;
  bb.clear();
  for (uint8_t addr : addrs) {
    if (shtStatus(addr, n, "")) { shtAddr = addr; return true; }
    // No valid status: put the sensor in a known state whatever it was doing, then ask again.
    // Break (0x3093) stops periodic acquisition, then a soft reset (0x30A2, 1.5 ms). Both NACK
    // while a measurement is running, so try for up to ~60 ms. A garbled command (e.g. from a
    // reset mid-transaction) can leave it measuring, and it acknowledges nothing while it measures.
    bool any = false;
    for (int t = 0; t < 4; ++t) {
      const bool brk = shtCommand(addr, 0x3093);
      delay(2);
      const bool rst = shtCommand(addr, 0x30A2);
      any |= brk || rst;
      if (brk && rst) break;
      delay(15);
    }
    if (!any) continue;  // nothing answers at this address
    delay(2);
    if (shtStatus(addr, n, " after reset")) { shtAddr = addr; return true; }
  }
  shtAddr = 0;
  return false;
}
bool shtRead() {
  StageScope stage(ST_I2C);
  bb.clear();
  uint8_t b[6] = {0, 0, 0, 0, 0, 0};
  // Single shot, high repeatability, no clock stretching; one retry if it is busy (NACK).
  // The result is polled: the read header is NACKed until the measurement is done (15.5 ms max
  // for a genuine SHT31), so try every 10 ms for up to ~110 ms and log how long it took. (A faulty
  // part on Nov 16 never finished; polling it for 1.5 s only cost time.) Counting continues in
  // interrupts meanwhile.
  const uint32_t t0 = millis();
  if (!shtCommand(shtAddr, 0x2400) && (delay(16), !shtCommand(shtAddr, 0x2400))) {
    snprintf(shtProbeLog, sizeof(shtProbeLog), "read: command NACK%s%s", bb.fault ? " " : "", bb.fault ? bb.why : "");
    return false;
  }
  delay(10);
  bool rd = false;
  for (int i = 0; i < 10 && !(rd = bb.read(shtAddr, b, 6)); ++i) delay(10);
  const bool ok = rd && sht31Decode(b, shtDeciK, shtPermille);
  snprintf(shtProbeLog, sizeof(shtProbeLog), "read: %s after %lu ms, %02x%02x/%02x %02x%02x/%02x%s%s",
           !rd ? "no data (NACK)" : ok ? "ok" : "CRC error", (unsigned long)(millis() - t0),
           b[0], b[1], b[2], b[3], b[4], b[5], bb.fault ? " " : "", bb.fault ? bb.why : "");
  return ok;
}
void shtService() {
  if (!i2cReady()) return;
  const uint32_t ms = wallMs();
  if (!shtAddr) {
    if (shtProbeMs && ms - shtProbeMs < BARO_RETRY_MS) return;
    if (!shtBegin()) return;
    shtReadMs = ms - SHT_EVERY_MS;
  }
  if (shtHave && ms - shtReadMs < SHT_EVERY_MS) return;
  shtReadMs = ms;
  if (shtRead()) { shtHave = true; shtGoodMs = ms; shtFailsInRow = 0; }
  else if (++shtErrors, ++shtFailsInRow >= SHT_FAILS_TO_PROBE || ms - shtGoodMs > BARO_STALE_MS) shtAddr = 0; // re-probe later
}
void i2cProbe() {
  StageScope stage(ST_I2C);
  char msg[160];
  const bool sda = (pinMode(PIN_WIRE_SDA, INPUT_PULLUP), digitalRead(PIN_WIRE_SDA));
  const bool scl = (pinMode(PIN_WIRE_SCL, INPUT_PULLUP), digitalRead(PIN_WIRE_SCL));
  // Without the internal pull-ups: do the breakouts pull the lines up themselves?
  pinMode(PIN_WIRE_SDA, INPUT); pinMode(PIN_WIRE_SCL, INPUT); delayMicroseconds(200);
  const bool sdaExt = digitalRead(PIN_WIRE_SDA), sclExt = digitalRead(PIN_WIRE_SCL);
  snprintf(msg, sizeof(msg), "# i2c-probe idle: SDA=%d SCL=%d (internal pull-ups on); external pull-ups only: SDA=%d SCL=%d",
           sda, scl, sdaExt, sclExt);
  logLine(msg);
  for (uint8_t a = 1; a < 127; ++a) {
    bb.fault = false;
    bb.start(); const bool ack = !bb.fault && bb.writeByte(a << 1); bb.stop();
    if (bb.fault) { snprintf(msg, sizeof(msg), "# i2c-probe 0x%02x: FAULT %s", a, bb.why); logLine(msg); break; }
    if (ack) { snprintf(msg, sizeof(msg), "# i2c-probe 0x%02x: ACK", a); logLine(msg); }
    wdtKick();
  }
  // SHT31 status register (0xF32D), CRC checked: proves full two-way communication.
  static const uint8_t shtAddrs[] = {0x44, 0x45};
  for (uint8_t a : shtAddrs) {
    bb.fault = false;
    bb.start(); bool ok = bb.writeByte(a << 1) && bb.writeByte(0xF3) && bb.writeByte(0x2D); bb.stop();
    if (!ok || bb.fault) continue;
    delayMicroseconds(500);
    bb.start(); ok = bb.writeByte(a << 1 | 1);
    uint8_t b[3] = {0, 0, 0};
    if (ok) { b[0] = bb.readByte(true); b[1] = bb.readByte(true); b[2] = bb.readByte(false); }
    bb.stop();
    snprintf(msg, sizeof(msg), "# i2c-probe SHT31 0x%02x status %02x%02x crc %02x %s%s", a, b[0], b[1], b[2],
             ok && sht31Crc(b, 2) == b[2] ? "OK" : "BAD", bb.fault ? bb.why : "");
    logLine(msg);
  }
  bb.rel(I2cLine::SDA); bb.rel(I2cLine::SCL);
}
void i2cScan() {
  StageScope stage(ST_I2C);
  char msg[200] = "# i2c devices:"; size_t n = strlen(msg);
  for (uint8_t a = 1; a < 127; ++a) {
    if (bb.write(a, nullptr, 0) && n + 6 < sizeof(msg)) n += snprintf(msg + n, sizeof(msg) - n, " 0x%02x", a);
    wdtKick();
  }
  if (n == strlen("# i2c devices:")) strcat(msg, " none");
  logLine(msg);
}
