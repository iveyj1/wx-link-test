#pragma once
// nRF52840 timebase and reed-style edge input, independent of the Arduino core's own use of
// RTC1 (FreeRTOS tick) and GPIOTE IN channels (attachInterrupt).
//
//  * RTC2, PRESCALER 0: 32768 ticks/s (the same unit as the Feather firmware), 24-bit counter
//    extended to 64 bits by counting overflows (every 512 s).
//  * Edge input: GPIO SENSE in LATCH mode -> GPIOTE PORT event -> PPI -> EGU3 interrupt.
//    PORT/SENSE costs no extra sleep current (unlike GPIOTE IN channels), and the core's
//    GPIOTE_IRQHandler only services IN channels, so the PORT event is routed through PPI
//    to EGU3, which has its own interrupt. Edges are fed to the shared PulseCounter, with
//    the debounce deadline on RTC2 CC[0].
#include <Arduino.h>
#include "PulseCounter.h"

namespace tb {

constexpr uint32_t HZ = 32768;
constexpr uint8_t PPI_CH = 4;           // the S140 SoftDevice reserves channels 17..31
constexpr uint8_t IRQ_PRIO = 2;         // RTC2 and EGU3 share a priority: no mutual preemption

volatile uint32_t overflows = 0;
volatile uint64_t rawEdges = 0;
volatile bool lastLevel = true;
uint8_t edgePin = 0xff;
PulseCounter pulses;

// Interrupt-safe 64-bit tick count.
inline uint64_t ticks() {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t c = NRF_RTC2->COUNTER, o = overflows;
  // Overflow happened but its interrupt hasn't run yet.
  if (NRF_RTC2->EVENTS_OVRFLW && c < 0x800000) ++o;
  __set_PRIMASK(primask);
  return (uint64_t(o) << 24) | c;
}

inline bool pinHigh(uint8_t pin) {
  NRF_GPIO_Type *port = pin < 32 ? NRF_P0 : NRF_P1;
  return (port->IN >> (pin & 31)) & 1;
}

// Called with RTC2/EGU3 priority (or interrupts masked).
inline void debounceArm() {
  if (!pulses.pending) { NRF_RTC2->INTENCLR = RTC_INTENSET_COMPARE0_Msk; return; }
  uint64_t now = ticks(), due = pulses.changedAt + PulseCounter::debounceTicks;
  if (due < now + 2) due = now + 2;
  NRF_RTC2->EVENTS_COMPARE[0] = 0;
  NRF_RTC2->CC[0] = uint32_t(due) & 0xffffff;
  NRF_RTC2->INTENSET = RTC_INTENSET_COMPARE0_Msk;
}

inline void begin() {
  NRF_RTC2->TASKS_STOP = 1;
  NRF_RTC2->PRESCALER = 0;
  NRF_RTC2->TASKS_CLEAR = 1;
  NRF_RTC2->EVENTS_OVRFLW = 0;
  NRF_RTC2->INTENSET = RTC_INTENSET_OVRFLW_Msk;
  NVIC_SetPriority(RTC2_IRQn, IRQ_PRIO);
  NVIC_ClearPendingIRQ(RTC2_IRQn);
  NVIC_EnableIRQ(RTC2_IRQn);
  NRF_RTC2->TASKS_START = 1;
}

// loopback = true: drive the pin as an output with its input buffer connected, so firmware
// can generate test pulses on a pin with nothing attached. false: input with pull-up (reed).
inline void edgeBegin(uint8_t pin, bool loopback) {
  edgePin = pin;
  NRF_GPIO_Type *port = pin < 32 ? NRF_P0 : NRF_P1;
  const uint32_t bit = 1u << (pin & 31);
  if (loopback) port->OUTSET = bit;
  const bool high = loopback ? true : pinHigh(pin);
  port->PIN_CNF[pin & 31] =
      ((loopback ? GPIO_PIN_CNF_DIR_Output : GPIO_PIN_CNF_DIR_Input) << GPIO_PIN_CNF_DIR_Pos) |
      (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
      ((loopback ? GPIO_PIN_CNF_PULL_Disabled : GPIO_PIN_CNF_PULL_Pullup) << GPIO_PIN_CNF_PULL_Pos) |
      (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
      ((high ? GPIO_PIN_CNF_SENSE_Low : GPIO_PIN_CNF_SENSE_High) << GPIO_PIN_CNF_SENSE_Pos);
  lastLevel = pinHigh(pin);
  pulses.begin(lastLevel);
  port->DETECTMODE = GPIO_DETECTMODE_DETECTMODE_LDETECT;
  port->LATCH = bit;
  NRF_PPI->CH[PPI_CH].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_PORT;
  NRF_PPI->CH[PPI_CH].TEP = (uint32_t)&NRF_EGU3->TASKS_TRIGGER[0];
  NRF_PPI->CHENSET = 1u << PPI_CH;
  NRF_EGU3->EVENTS_TRIGGERED[0] = 0;
  NRF_EGU3->INTENSET = 1;
  NVIC_SetPriority(SWI3_EGU3_IRQn, IRQ_PRIO);
  NVIC_ClearPendingIRQ(SWI3_EGU3_IRQn);
  NVIC_EnableIRQ(SWI3_EGU3_IRQn);
}

}  // namespace tb

extern "C" void RTC2_IRQHandler() {
  if (NRF_RTC2->EVENTS_OVRFLW) { NRF_RTC2->EVENTS_OVRFLW = 0; ++tb::overflows; }
  if (NRF_RTC2->EVENTS_COMPARE[0]) {
    NRF_RTC2->EVENTS_COMPARE[0] = 0;
    if (tb::edgePin != 0xff) {
      tb::pulses.timer(tb::pinHigh(tb::edgePin), tb::ticks());
      tb::debounceArm();
    }
  }
  __DSB();
}

extern "C" void SWI3_EGU3_IRQHandler() {
  NRF_EGU3->EVENTS_TRIGGERED[0] = 0;
  NRF_GPIOTE->EVENTS_PORT = 0;
  const uint8_t pin = tb::edgePin;
  NRF_GPIO_Type *port = pin < 32 ? NRF_P0 : NRF_P1;
  const uint32_t bit = 1u << (pin & 31);
  for (int guard = 0; guard < 16; ++guard) {
    const bool high = tb::pinHigh(pin);
    // Sense the opposite level next, then clear the latch.
    uint32_t cnf = port->PIN_CNF[pin & 31] & ~GPIO_PIN_CNF_SENSE_Msk;
    port->PIN_CNF[pin & 31] = cnf | ((high ? GPIO_PIN_CNF_SENSE_Low : GPIO_PIN_CNF_SENSE_High) << GPIO_PIN_CNF_SENSE_Pos);
    port->LATCH = bit;
    if (high != tb::lastLevel) {
      tb::lastLevel = high;
      ++tb::rawEdges;
      tb::pulses.edge(high, tb::ticks());
    }
    if (tb::pinHigh(pin) == high) break;  // no further change while reconfiguring
  }
  tb::debounceArm();
  __DSB();
}
