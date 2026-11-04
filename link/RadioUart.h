#pragma once
#include <Arduino.h>
#include "wiring_private.h"

// Feather M0 D0/D1 = SERCOM0 RX pad 3 / TX pad 2.
// Keep the Arduino RX ISR/ring, but NEVER use its unbounded TX ring/write/flush.
struct RadioUart {
  uint32_t baud = 115200, timeouts = 0, recoveries = 0, inferredCompletions = 0;
  uint32_t lastFlags = 0, lastStatus = 0, lastCtrlA = 0, lastSync = 0;
  bool ready = false, injectCompletionTimeout = false, lastInjected = false;
  const char *lastFault = "none";

  template<class Predicate>
  bool wait(Predicate predicate, uint32_t start, uint32_t timeout) {
    while (!predicate()) {
      if (uint32_t(millis() - start) >= timeout) return false;
    }
    return true;
  }
  void fault(const char *reason, bool injected=false) {
    ++timeouts; ready = false;
    lastFault = reason; lastInjected = injected;
    lastFlags = SERCOM0->USART.INTFLAG.reg;
    lastStatus = SERCOM0->USART.STATUS.reg;
    lastCtrlA = SERCOM0->USART.CTRLA.reg;
    lastSync = SERCOM0->USART.SYNCBUSY.reg;
  }
  bool begin(uint32_t newBaud) {
    StageScope stage(ST_UART_INIT);
    ready = false; baud = newBaud;
    NVIC_DisableIRQ(SERCOM0_IRQn);
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM0;
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_SERCOM0_CORE | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_CLKEN;
    if (!wait([] { return !GCLK->STATUS.bit.SYNCBUSY; }, millis(), 20)) {
      fault("clock_sync"); return false;
    }
    SERCOM0->USART.CTRLA.reg = SERCOM_USART_CTRLA_SWRST;
    if (!wait([] { return !SERCOM0->USART.CTRLA.bit.SWRST && !SERCOM0->USART.SYNCBUSY.bit.SWRST; }, millis(), 20)) {
      fault("reset_sync"); return false;
    }
    pinPeripheral(PIN_SERIAL1_RX, g_APinDescription[PIN_SERIAL1_RX].ulPinType);
    pinPeripheral(PIN_SERIAL1_TX, g_APinDescription[PIN_SERIAL1_TX].ulPinType);
    SERCOM0->USART.CTRLA.reg = SERCOM_USART_CTRLA_MODE(UART_INT_CLOCK) |
        SERCOM_USART_CTRLA_SAMPR(SAMPLE_RATE_x16) | SERCOM_USART_CTRLA_DORD |
        SERCOM_USART_CTRLA_TXPO(PAD_SERIAL1_TX) | SERCOM_USART_CTRLA_RXPO(PAD_SERIAL1_RX);
    // Same fractional 16x baud calculation as the Adafruit SAMD core (48 MHz GCLK0).
    uint32_t divisor = (F_CPU * 8UL) / (16UL * baud);
    SERCOM0->USART.BAUD.FRAC.BAUD = divisor / 8;
    SERCOM0->USART.BAUD.FRAC.FP = divisor % 8;
    SERCOM0->USART.CTRLB.reg = SERCOM_USART_CTRLB_TXEN | SERCOM_USART_CTRLB_RXEN;
    if (!wait([] { return !SERCOM0->USART.SYNCBUSY.bit.CTRLB; }, millis(), 20)) {
      fault("frame_sync"); return false;
    }
    while (Serial1.available()) Serial1.read();
    SERCOM0->USART.INTENSET.reg = SERCOM_USART_INTENSET_RXC | SERCOM_USART_INTENSET_ERROR;
    SERCOM0->USART.CTRLA.bit.ENABLE = 1;
    if (!wait([] { return !SERCOM0->USART.SYNCBUSY.bit.ENABLE; }, millis(), 20)) {
      fault("enable_sync"); return false;
    }
    NVIC_ClearPendingIRQ(SERCOM0_IRQn);
    NVIC_SetPriority(SERCOM0_IRQn, SERCOM_NVIC_PRIORITY);
    NVIC_EnableIRQ(SERCOM0_IRQn);
    ready = true;
    return true;
  }
  bool failAndRecover(const char *reason, bool injected=false) {
    fault(reason,injected);
    if (begin(baud)) ++recoveries;
    // Caller treats this attempt as failed, even when UART reinitialization worked.
    return false;
  }
  bool line(const char *text) {
    StageScope stage(ST_RADIO_WRITE);
    if (!ready && !begin(baud)) return false;
    const bool injected = injectCompletionTimeout;
    injectCompletionTimeout = false;
    const size_t length = strlen(text);
    const uint32_t start = millis();
    // Time on wire at current baud plus a generous interrupt-latency margin.
    const uint32_t timeout = ((length + 2) * 10000UL + baud - 1) / baud + 100;
    for (size_t i=0; i<length+2; ++i) {
      if (!wait([] { return SERCOM0->USART.INTFLAG.bit.DRE; }, start, timeout))
        return failAndRecover("tx_data_ready");
      // TX ring is never used; RX ISR cannot write DATA when that ring is empty.
      SERCOM0->USART.DATA.reg = i<length ? uint8_t(text[i]) : (i==length ? '\r' : '\n');
    }
    traceStage(ST_UART_COMPLETE);
    // The core's Serial1 RX/error ISR acknowledges errors with a read-modify-write of
    // INTFLAG (SERCOM.cpp acknowledgeUARTError), which can clear a set TXC. Do not rely
    // on TXC alone: once DRE is set again the final byte is in the shift register and
    // finishes within one character time.
    if (!wait([injected] { return !injected && SERCOM0->USART.INTFLAG.bit.DRE; }, start, timeout))
      return failAndRecover("tx_complete",injected);
    const uint32_t shiftStart = micros();
    const uint32_t characterUs = (10000000UL + baud - 1) / baud;
    while (!SERCOM0->USART.INTFLAG.bit.TXC) {
      if (uint32_t(micros() - shiftStart) >= 2 * characterUs + 50) { ++inferredCompletions; break; }
    }
    return true;
  }
};
