// SoftI2c against a simulated open-drain bus with one register-style device.
#include <cstdio>
#include <cstring>
#include "link/SoftI2c.h"

// Wired-AND bus: a line is high only if neither the master nor the device pulls it low.
struct Bus {
  static bool mSda, mScl;        // master releases (true) or pulls low (false)
  static bool dSda;              // device's SDA drive
  static uint64_t nowUs, sclLowUntil, stretchUs;
  static bool sclStuck, sdaStuck;
  // Device: address 0x44; write stores bytes; read returns them back; optional clock stretch after
  // the address byte.
  static uint8_t mem[8]; static int memLen, bit, byteIndex; static uint8_t shift;
  static bool addressed, reading, active, ackPhase, lastScl, lastSda;
  static int starts, stops;
  static bool sda() { return mSda && dSda && !sdaStuck; }
  static bool scl() { return mScl && !sclStuck && nowUs >= sclLowUntil; }
  static void step() {  // run the device on every change of the lines
    const bool s = sda(), c = scl();
    if (lastScl && c && lastSda && !s) {          // START
      ++starts; active = true; addressed = false; bit = 0; shift = 0; ackPhase = false; dSda = true; byteIndex = 0;
    } else if (lastScl && c && !lastSda && s) {   // STOP
      ++stops; active = false; dSda = true;
    } else if (active && !lastScl && c) {         // rising SCL: sample
      if (!ackPhase) {
        if (!(addressed && reading)) shift = uint8_t(shift << 1 | s);
        ++bit;
      } else if (addressed && reading && s) { active = false; }  // master NACK ends the read
    } else if (active && lastScl && !c) {         // falling SCL: change SDA
      if (ackPhase) { ackPhase = false; dSda = true; bit = 0;
        if (addressed && reading) dSda = mem[byteIndex % 8] >> 7 & 1;
      } else if (bit == 8) {
        ackPhase = true;
        if (!addressed) {
          if (shift >> 1 == 0x44) { addressed = true; reading = shift & 1; dSda = false;
            if (stretchUs) sclLowUntil = nowUs + stretchUs; }
          else { active = false; dSda = true; }
        } else if (!reading) { if (byteIndex < 8) mem[byteIndex++] = shift; memLen = byteIndex; dSda = false; }
        else { dSda = true; ++byteIndex; }        // release for the master's ACK/NACK
        shift = 0;
      } else if (addressed && reading) dSda = mem[byteIndex % 8] >> (7 - bit) & 1;
    }
    lastScl = c; lastSda = s;
  }
  static void low(I2cLine l) { (l == I2cLine::SDA ? mSda : mScl) = false; step(); }
  static void release(I2cLine l) { (l == I2cLine::SDA ? mSda : mScl) = true; step(); }
  static bool read(I2cLine l) { step(); return l == I2cLine::SDA ? sda() : scl(); }
  static void delayUs(uint32_t us) { nowUs += us; step(); }
  static void reset() {
    mSda = mScl = dSda = true; nowUs = sclLowUntil = stretchUs = 0; sclStuck = sdaStuck = false;
    memLen = bit = byteIndex = 0; shift = 0; addressed = reading = active = ackPhase = false;
    lastScl = lastSda = true; starts = stops = 0; std::memset(mem, 0, sizeof mem);
  }
};
bool Bus::mSda, Bus::mScl, Bus::dSda, Bus::sclStuck, Bus::sdaStuck;
uint64_t Bus::nowUs, Bus::sclLowUntil, Bus::stretchUs;
uint8_t Bus::mem[8]; int Bus::memLen, Bus::bit, Bus::byteIndex; uint8_t Bus::shift;
bool Bus::addressed, Bus::reading, Bus::active, Bus::ackPhase, Bus::lastScl, Bus::lastSda;
int Bus::starts, Bus::stops;

static int fails = 0;
static void check(bool ok, const char *what) { if (!ok) { std::printf("FAIL: %s\n", what); ++fails; } }

int main() {
  SoftI2c<Bus> i2c;
  const uint8_t cmd[2] = {0xF3, 0x2D};
  uint8_t in[2];

  Bus::reset();
  check(i2c.write(0x44, cmd, 2), "write to a present device is acknowledged");
  check(Bus::memLen == 2 && Bus::mem[0] == 0xF3 && Bus::mem[1] == 0x2D, "device received both bytes");
  check(Bus::starts == 1 && Bus::stops == 1, "one START and one STOP per write");
  check(i2c.read(0x44, in, 2) && in[0] == 0xF3 && in[1] == 0x2D, "read returns the device's bytes");
  check(Bus::sda() && Bus::scl(), "bus idle high after a transaction");

  Bus::reset();
  check(!i2c.write(0x45, cmd, 2) && !i2c.fault, "absent address: NACK, not a bus fault");
  check(Bus::stops == 1 && Bus::sda() && Bus::scl(), "STOP sent and bus released after a NACK");

  Bus::reset(); Bus::stretchUs = 15000;  // an SHT31-style 15 ms stretch
  check(i2c.write(0x44, cmd, 2), "15 ms clock stretch is waited out");
  Bus::reset(); Bus::stretchUs = 30000;
  check(!i2c.write(0x44, cmd, 2) && i2c.fault, "30 ms stretch times out as a fault");
  check(std::strstr(i2c.why, "SCL") != nullptr, "fault names SCL");

  Bus::reset(); Bus::sclStuck = true;
  uint64_t t0 = Bus::nowUs;
  check(!i2c.write(0x44, cmd, 2) && i2c.fault, "SCL stuck low: fault");
  check(Bus::nowUs - t0 < 100000, "stuck SCL returns within 100 ms (no hang)");
  Bus::reset(); Bus::sdaStuck = true;
  check(!i2c.write(0x44, cmd, 2) && i2c.fault && std::strstr(i2c.why, "SDA"), "SDA stuck low: fault names SDA");

  // Bus clear: a device left mid-read (holding SDA low) is clocked out and sees a STOP.
  Bus::reset(); i2c.warmed = true;
  Bus::mem[0] = 0x00;  // it will drive zeros
  i2c.fault = false; i2c.start(); i2c.writeByte(0x44 << 1 | 1);  // addressed for read, then abandoned
  Bus::mScl = true; Bus::mSda = true; Bus::step();
  check(!Bus::sda(), "abandoned read: device holds SDA low");
  i2c.clear();
  check(Bus::sda() && Bus::scl() && !Bus::active, "bus clear frees SDA and ends the device's transaction");
  check(i2c.write(0x44, cmd, 2), "device answers normally after a bus clear");

  if (fails) return 1;
  std::puts("PASS: soft I2C write/read, NACK, 15 ms stretch, stretch timeout, stuck SCL/SDA without hanging, bus clear");
}
