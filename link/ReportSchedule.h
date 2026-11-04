#pragma once
#include <stdint.h>

// Access only with interrupts masked or from the RTC ISR.
struct ReportSchedule {
  static constexpr uint64_t hz = 32768;
  uint64_t sentCount = UINT64_MAX;
  uint64_t lastSuccess = 0;
  uint64_t notBefore = 0;
  bool retryPending = false;
  uint64_t deadline(uint64_t count) const {
    uint64_t wanted = (retryPending || count != sentCount) ? 0 : lastSuccess + 60 * hz;
    return wanted > notBefore ? wanted : notBefore;
  }
  bool due(uint64_t now, uint64_t count) const { return now >= deadline(count); }
  void finished(uint64_t now, uint64_t count, bool success) {
    // Limit from completion, not start: slow radio operations cannot cause bursts.
    notBefore = now + (success ? hz : 5 * hz);
    retryPending = !success;
    if (success) { sentCount = count; lastSuccess = now; }
  }
};
