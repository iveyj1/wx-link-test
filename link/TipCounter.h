#pragma once
#include <stdint.h>

// Tipping-bucket rain gauge. Each tip closes the reed (D10 low) for about 100 ms, then it
// reopens: one debounced closure = one calibrated bucket of rain; the reopening is not
// counted. A gauge resting closed at boot counts its first new closure. Same debounce and serialization rules as
// PulseCounter: called with interrupts masked or from the RTC/EIC ISRs, 32768-Hz ticks.
struct TipCounter {
  static constexpr uint32_t debounceTicks = 328; // 10.0098 ms, as for the wind reed
  bool stable = true, candidate = true, pending = false, haveTip = false;
  uint64_t changedAt = 0, tips = 0, last = 0, interval = 0;
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
    if (stable == candidate) return; // bounce returned to the old level: no tip
    stable = candidate;
    if (stable) return; // reopening after the ~100 ms closure
    ++tips;
    // Interval between the last two tips; 0 until two have been seen since boot.
    interval = haveTip ? changedAt - last : 0;
    last = changedAt;
    haveTip = true;
  }
};
