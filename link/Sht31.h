#pragma once
#include <stdint.h>

// SHT31 (Sensirion SHT3x-DIS datasheet) helpers, free of I/O so they can be tested natively.
// Single-shot, high repeatability, no clock stretching: write 0x24 0x00, wait >= 15.5 ms, read
// 6 bytes: T msb, T lsb, T crc, RH msb, RH lsb, RH crc.

// CRC-8, polynomial 0x31, init 0xFF (datasheet 4.12; example 0xBEEF -> 0x92).
inline uint8_t sht31Crc(const uint8_t *data, uint8_t n) {
  uint8_t crc = 0xff;
  for (uint8_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) crc = crc & 0x80 ? uint8_t(crc << 1 ^ 0x31) : uint8_t(crc << 1);
  }
  return crc;
}

// Temperature in 0.1 K (T = -45 + 175 * raw / 65535 deg C), rounded: 2282..4032.
inline uint16_t sht31DeciK(uint16_t raw) {
  return uint16_t(2282 + (uint32_t(1750) * raw + 32767) / 65535);
}

// Relative humidity in 0.1 % (RH = 100 * raw / 65535 %), rounded: 0..1000.
inline uint16_t sht31Permille(uint16_t raw) {
  return uint16_t((uint32_t(1000) * raw + 32767) / 65535);
}

// Both words and their CRCs from a 6-byte read; false on a CRC mismatch.
inline bool sht31Decode(const uint8_t b[6], uint16_t &deciK, uint16_t &permille) {
  if (sht31Crc(b, 2) != b[2] || sht31Crc(b + 3, 2) != b[5]) return false;
  deciK = sht31DeciK(uint16_t(b[0] << 8 | b[1]));
  permille = sht31Permille(uint16_t(b[3] << 8 | b[4]));
  return true;
}
