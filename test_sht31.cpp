// SHT31 CRC and conversions against the datasheet (SHT3x-DIS 4.12, 4.13).
#include <cstdio>
#include "link/Sht31.h"
static int fails = 0;
static void check(bool ok, const char *what) { if (!ok) { std::printf("FAIL: %s\n", what); ++fails; } }
int main() {
  const uint8_t beef[] = {0xbe, 0xef};
  check(sht31Crc(beef, 2) == 0x92, "CRC example 0xBEEF -> 0x92");
  check(sht31DeciK(0) == 2282 && sht31DeciK(0xffff) == 4032, "T range -45..130 C");
  check(sht31DeciK(0x6666) == 2732 + 250, "T 0x6666 = 25.0 C");  // -45 + 175 * 0.4 = 25
  check(sht31Permille(0) == 0 && sht31Permille(0xffff) == 1000, "RH range 0..100 %");
  check(sht31Permille(0x8000) == 500, "RH 0x8000 = 50.0 %");
  uint8_t frame[6] = {0x66, 0x66, 0, 0x80, 0x00, 0};
  frame[2] = sht31Crc(frame, 2); frame[5] = sht31Crc(frame + 3, 2);
  uint16_t dk = 0, pm = 0;
  check(sht31Decode(frame, dk, pm) && dk == 2982 && pm == 500, "decode good frame");
  frame[4] ^= 1;
  check(!sht31Decode(frame, dk, pm), "reject bad CRC");
  if (fails) return 1;
  std::puts("PASS: SHT31 CRC (0xBEEF -> 0x92), temperature and humidity conversions, CRC rejection");
}
