#pragma once
#include <Arduino.h>

enum Stage : uint32_t {
  ST_OUTSIDE=0, ST_SETUP, ST_LOOP, ST_USB_SERVICE, ST_REPORT,
  ST_RADIO_CONFIG, ST_WAKE_WRITE, ST_WAKE_FLUSH, ST_WAKE_DELAY,
  ST_WAKE_DRAIN, ST_RADIO_WRITE, ST_RADIO_WAIT, ST_USB_LOG,
  ST_CLOCK_SYNC, ST_RTC_IRQ, ST_REED_IRQ, ST_IDLE,
  ST_SNAPSHOT, ST_SCHEDULE, ST_MANUAL_RESET, ST_UART_COMPLETE, ST_UART_INIT, ST_BARO,
  ST_LAST = ST_BARO
};
struct TraceRecord {
  uint32_t magic, stage, inverse, parent, sinceMs, session;
  char command[72];
};
constexpr uint32_t TRACE_MAGIC = 0x57444931; // WDI1
volatile TraceRecord traceRecord __attribute__((section(".noinit")));
TraceRecord previousTrace;
bool previousTraceValid = false;

const char *stageName(uint32_t stage) {
  static const char *names[] = {"outside", "setup", "loop", "usb_service", "report",
    "radio_config", "wake_write", "wake_flush", "wake_delay", "wake_drain",
    "radio_write", "radio_wait", "usb_log", "clock_sync", "rtc_irq", "reed_irq",
    "idle", "snapshot", "schedule", "manual_reset", "uart_complete", "uart_init", "baro"};
  return stage <= ST_LAST ? names[stage] : "invalid";
}
void traceBegin(uint8_t cause) {
  previousTraceValid = !(cause & (PM_RCAUSE_POR | PM_RCAUSE_BOD12 | PM_RCAUSE_BOD33)) &&
      traceRecord.magic == TRACE_MAGIC && traceRecord.stage <= ST_LAST &&
      traceRecord.stage == ~traceRecord.inverse;
  if (previousTraceValid) {
    previousTrace.magic = traceRecord.magic;
    previousTrace.stage = traceRecord.stage;
    previousTrace.inverse = traceRecord.inverse;
    previousTrace.parent = traceRecord.parent;
    previousTrace.sinceMs = traceRecord.sinceMs;
    previousTrace.session = traceRecord.session;
    for (unsigned i=0; i<sizeof(previousTrace.command); ++i)
      previousTrace.command[i] = traceRecord.command[i];
    previousTrace.command[sizeof(previousTrace.command)-1] = 0;
  }
  traceRecord.magic = TRACE_MAGIC;
  traceRecord.stage = ST_OUTSIDE; traceRecord.inverse = ~uint32_t(ST_OUTSIDE);
  traceRecord.parent = ST_OUTSIDE; traceRecord.sinceMs = 0;
  traceRecord.session = 0; traceRecord.command[0] = 0;
}
void traceStage(uint32_t stage) {
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  traceRecord.stage = stage;
  traceRecord.inverse = ~stage;
  traceRecord.sinceMs = millis();
  __DMB(); __set_PRIMASK(mask);
}
// Nested scopes preserve both the main-loop and interrupt context. No I/O or RTC reads.
struct StageScope {
  uint32_t stage, parent, sinceMs;
  explicit StageScope(uint32_t next) {
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    stage = traceRecord.stage; parent = traceRecord.parent; sinceMs = traceRecord.sinceMs;
    traceRecord.parent = stage;
    traceStage(next);
    __set_PRIMASK(mask);
  }
  ~StageScope() {
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    traceRecord.stage = stage; traceRecord.inverse = ~stage;
    traceRecord.parent = parent; traceRecord.sinceMs = sinceMs;
    __DMB(); __set_PRIMASK(mask);
  }
};
void traceCommand(const char *command) {
  unsigned i = 0;
  for (; command[i] && i < sizeof(traceRecord.command)-1; ++i) traceRecord.command[i] = command[i];
  traceRecord.command[i] = 0;
}
