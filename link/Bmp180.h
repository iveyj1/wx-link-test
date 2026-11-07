#pragma once
#include <stdint.h>

// BMP180 compensation (Bosch datasheet BST-BMP180-DS000, section 3.5), kept free of I/O so
// it can be tested natively. Inputs: the 11 calibration words from EEPROM 0xAA..0xBF, the raw
// temperature UT and raw pressure UP (already shifted right by 8 - oss).
struct Bmp180Cal {
  int16_t ac1, ac2, ac3; uint16_t ac4, ac5, ac6; int16_t b1, b2, mb, mc, md;
  // Erased (0x0000/0xFFFF) words mean a bad read or a different chip.
  bool plausible() const {
    const uint16_t w[] = {uint16_t(ac1),uint16_t(ac2),uint16_t(ac3),ac4,ac5,ac6,
                          uint16_t(b1),uint16_t(b2),uint16_t(mb),uint16_t(mc),uint16_t(md)};
    for (uint16_t v : w) if (v == 0 || v == 0xffff) return false;
    return true;
  }
};

// Temperature in 0.1 deg C and pressure in Pa.
inline void bmp180Compensate(const Bmp180Cal &c, int32_t ut, int32_t up, uint8_t oss,
                             int32_t &deciC, int32_t &pa) {
  int32_t x1 = ((ut - int32_t(c.ac6)) * int32_t(c.ac5)) >> 15;
  int32_t x2 = (int32_t(c.mc) << 11) / (x1 + c.md);
  int32_t b5 = x1 + x2;
  deciC = (b5 + 8) >> 4;
  int32_t b6 = b5 - 4000;
  x1 = (int32_t(c.b2) * ((b6 * b6) >> 12)) >> 11;
  x2 = (int32_t(c.ac2) * b6) >> 11;
  int32_t x3 = x1 + x2;
  int32_t b3 = (((int32_t(c.ac1) * 4 + x3) << oss) + 2) / 4;
  x1 = (int32_t(c.ac3) * b6) >> 13;
  x2 = (int32_t(c.b1) * ((b6 * b6) >> 12)) >> 16;
  x3 = ((x1 + x2) + 2) >> 2;
  uint32_t b4 = (uint32_t(c.ac4) * uint32_t(x3 + 32768)) >> 15;
  uint32_t b7 = (uint32_t(up) - b3) * (50000UL >> oss);
  int32_t p = b7 < 0x80000000UL ? int32_t((b7 * 2) / b4) : int32_t((b7 / b4) * 2);
  x1 = (p >> 8) * (p >> 8);
  x1 = (x1 * 3038) >> 16;
  x2 = (-7357 * p) >> 16;
  pa = p + ((x1 + x2 + 3791) >> 4);
}
