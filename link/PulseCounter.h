#pragma once
#include <stdint.h>

// Called with serialized access. Time is monotonic 32768-Hz RTC ticks.
struct PulseCounter {
  static constexpr uint32_t debounceTicks = 328; // 10.0098 ms
  bool stable = true, candidate = true, pending = false, havePulse = false;
  uint64_t changedAt = 0, count = 0, last = 0, period = 0;
  // First closure is the reference; closures 3, 5, ... complete revolutions.
  uint64_t revolutions = 0, revolutionPeriod = 0, lastRevolution = 0;
  uint64_t intervalA = 0, intervalB = 0, pendingA = 0;
  void begin(bool high) { stable = candidate = high; }
  void edge(bool high, uint64_t now) {
    candidate = high;
    changedAt = now;
    pending = true;
  }
  void timer(bool high, uint64_t now) {
    if (!pending) return;
    if (high != candidate) { edge(high, now); return; }
    if (now - changedAt < debounceTicks) return;
    pending = false;
    if (stable == candidate) return;
    stable = candidate;
    if (!stable) {
      ++count;
      if (havePulse) {
        period = changedAt - last;
        if ((count & 1) == 0) {
          pendingA = period;
        } else {
          intervalA = pendingA;
          intervalB = period;
          revolutionPeriod = intervalA + intervalB;
          lastRevolution = changedAt;
          ++revolutions;
        }
      }
      last = changedAt;
      havePulse = true;
    }
  }
};
