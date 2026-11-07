// BMP180 compensation against the datasheet worked example (BST-BMP180-DS000, 3.5).
#include <cstdio>
#include <cstdlib>
#include "link/Bmp180.h"
int main() {
  Bmp180Cal c = {408, -72, -14383, 32741, 32757, 23153, 6190, 4, -32768, -8711, 2868};
  int32_t t, pa;
  bmp180Compensate(c, 27898, 23843, 0, t, pa);
  if (t != 150 || pa != 69964) { std::printf("FAIL: t=%ld pa=%ld\n", (long)t, (long)pa); return 1; }
  if (!c.plausible()) { std::puts("FAIL: plausible"); return 1; }
  Bmp180Cal erased = c; erased.ac4 = 0xffff;
  if (erased.plausible()) { std::puts("FAIL: erased"); return 1; }
  std::puts("PASS: BMP180 datasheet example 15.0 C, 69964 Pa; erased calibration rejected");
}
