#pragma once
#include <stdint.h>

// Software (bit-banged) I2C master, free of hardware access so it can be tested natively
// (test_softi2c.cpp). The SAMD21 SERCOM Wire driver has no timeouts and locked the board up in
// setup() with a faulty SHT31 on the bus (Nov 16). Here every wait for a released line times out,
// so a bad bus costs an error, never a hang.
//
// Pins supplies the hardware as static functions:
//   void low(I2cLine)      drive the line low
//   void release(I2cLine)  let the pull-up take it high (open drain)
//   bool read(I2cLine)     the line's level
//   void delayUs(uint32_t)
enum class I2cLine : uint8_t { SDA, SCL };

template <class Pins> struct SoftI2c {
  static constexpr uint32_t halfUs = 10;          // ~50 kHz
  static constexpr uint32_t stretchPolls = 2000;  // x 10 us = 20 ms of clock stretching
  bool fault = false;
  const char *why = "";
  bool warmed = false;

  void low(I2cLine l) { Pins::low(l); }
  void rel(I2cLine l) { Pins::release(l); }
  void half() { Pins::delayUs(halfUs); }
  bool sclHigh() {  // release SCL and wait out clock stretching
    rel(I2cLine::SCL);
    for (uint32_t i = 0; i < stretchPolls; ++i) { if (Pins::read(I2cLine::SCL)) return true; Pins::delayUs(10); }
    fault = true; why = "SCL held low (clock stretch timeout)"; return false;
  }
  void start() {
    rel(I2cLine::SDA); half(); if (!sclHigh()) return; half();
    if (!Pins::read(I2cLine::SDA)) { fault = true; why = "SDA low before START"; return; }
    low(I2cLine::SDA); half(); low(I2cLine::SCL); half();
  }
  void stop() { low(I2cLine::SDA); half(); sclHigh(); half(); rel(I2cLine::SDA); half(); }
  bool writeByte(uint8_t v) {  // true = ACK
    for (int i = 7; i >= 0; --i) {
      if (v >> i & 1) rel(I2cLine::SDA); else low(I2cLine::SDA);
      half(); if (!sclHigh()) return false; half(); low(I2cLine::SCL);
    }
    rel(I2cLine::SDA); half(); if (!sclHigh()) return false;
    const bool ack = !Pins::read(I2cLine::SDA); half(); low(I2cLine::SCL);
    return ack;
  }
  uint8_t readByte(bool ack) {
    uint8_t v = 0; rel(I2cLine::SDA);
    for (int i = 0; i < 8; ++i) {
      half(); if (!sclHigh()) return 0;
      v = uint8_t(v << 1 | Pins::read(I2cLine::SDA)); half(); low(I2cLine::SCL);
    }
    if (ack) low(I2cLine::SDA);
    half(); sclHigh(); half(); low(I2cLine::SCL); rel(I2cLine::SDA);
    return v;
  }
  // Bus clear (I2C spec 3.1.16): with SDA released, 9 clocks let a device stuck mid-byte finish and
  // see a NACK, then a STOP returns every device to idle. Done before each sensor access.
  void clear() {
    if (!warmed) {
      // First use after boot: the pins have just gone from reset (floating) to pulled up, and that
      // transition can leave a device out of step. Settle, then send a throwaway START/STOP.
      warmed = true;
      rel(I2cLine::SDA); rel(I2cLine::SCL); Pins::delayUs(2000);
      start(); stop(); Pins::delayUs(1000);
    }
    rel(I2cLine::SDA);
    for (int i = 0; i < 9; ++i) { low(I2cLine::SCL); half(); if (!sclHigh()) return; half(); }
    low(I2cLine::SCL); half(); stop();
  }
  // Whole transactions; false on NACK or bus fault (why says which).
  bool write(uint8_t addr, const uint8_t *d, uint8_t n, bool stopAfter = true) {
    fault = false; start();
    bool ok = !fault && writeByte(uint8_t(addr << 1));
    for (uint8_t i = 0; ok && i < n; ++i) ok = writeByte(d[i]);
    if (stopAfter || !ok) stop();
    return ok && !fault;
  }
  bool read(uint8_t addr, uint8_t *d, uint8_t n) {  // also a repeated START after write(.., false)
    fault = false; start();
    bool ok = !fault && writeByte(uint8_t(addr << 1 | 1));
    for (uint8_t i = 0; ok && i < n; ++i) d[i] = readByte(i + 1 < n);
    stop();
    return ok && !fault;
  }
};
