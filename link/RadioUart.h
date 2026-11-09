#pragma once
#include <Arduino.h>
#include "wiring_private.h"

// Feather M0 D0/D1 = SERCOM0 RX pad 3 / TX pad 2.
// Keep the Arduino RX ISR/ring, but NEVER use its unbounded TX ring/write/flush.
//
// TX goes out by DMA (channel 0, triggered by SERCOM0 DRE), so a command line leaves
// back-to-back whatever interrupts run meanwhile. The RYLR998 ends a command at an idle
// gap of about one character: when bytes were written by the CPU, a reed/RTC interrupt
// (several RTC syncs, a few hundred us) split lines into "+ERR=1" (no CR LF) and "+ERR=2"
// (no "AT") pairs, ~10% of reports while the rotor turned (bench, Nov 9).
// The RX interrupt also runs at a higher priority than EIC/RTC (set in begin()), so
// replies are not overrun ("+O", "+PARAMET,1,12").
static DmacDescriptor radioDmaDesc __attribute__((aligned(16)));
static DmacDescriptor radioDmaWriteback __attribute__((aligned(16)));
constexpr uint8_t RADIO_DMA_CHANNEL = 0;

struct RadioUart {
  uint32_t baud = 115200, timeouts = 0, recoveries = 0, inferredCompletions = 0;
  uint32_t lastFlags = 0, lastStatus = 0, lastCtrlA = 0, lastSync = 0;
  bool ready = false, injectCompletionTimeout = false, lastInjected = false, dmaReady = false;
  uint32_t dmaTimeouts = 0;
  char txBuffer[300];
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
    // Above EIC/RTC (priority 1): the RX ISR only moves a byte into the ring (a few us),
    // and a reed ISR must not hold it off for longer than two characters (174 us).
    NVIC_SetPriority(SERCOM0_IRQn, 0);
    NVIC_EnableIRQ(SERCOM0_IRQn);
    dmaBegin();
    ready = true;
    return true;
  }
  void dmaBegin() {
    if (dmaReady) return;
    PM->AHBMASK.reg |= PM_AHBMASK_DMAC;
    PM->APBBMASK.reg |= PM_APBBMASK_DMAC;
    DMAC->CTRL.reg = 0;
    DMAC->CTRL.reg = DMAC_CTRL_SWRST;
    while (DMAC->CTRL.bit.SWRST) {}
    DMAC->BASEADDR.reg = uint32_t(&radioDmaDesc);
    DMAC->WRBADDR.reg = uint32_t(&radioDmaWriteback);
    DMAC->CTRL.reg = DMAC_CTRL_DMAENABLE | DMAC_CTRL_LVLEN(0xf);
    DMAC->CHID.reg = DMAC_CHID_ID(RADIO_DMA_CHANNEL);
    DMAC->CHCTRLA.reg = 0;
    DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;
    while (DMAC->CHCTRLA.bit.SWRST) {}
    DMAC->CHCTRLB.reg = DMAC_CHCTRLB_LVL(0) | DMAC_CHCTRLB_TRIGSRC(SERCOM0_DMAC_ID_TX) |
                        DMAC_CHCTRLB_TRIGACT_BEAT;
    dmaReady = true;
  }
  void dmaStop() {
    DMAC->CHID.reg = DMAC_CHID_ID(RADIO_DMA_CHANNEL);
    DMAC->CHCTRLA.reg = 0;
    while (DMAC->CHCTRLA.bit.ENABLE) {}
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
    size_t length = strlen(text);
    if (length > sizeof(txBuffer) - 2) length = sizeof(txBuffer) - 2;
    memcpy(txBuffer, text, length);
    txBuffer[length] = '\r'; txBuffer[length + 1] = '\n';
    const uint32_t start = millis();
    // Time on wire at current baud plus a generous interrupt-latency margin.
    const uint32_t timeout = ((length + 2) * 10000UL + baud - 1) / baud + 100;
    if (!wait([] { return SERCOM0->USART.INTFLAG.bit.DRE; }, start, timeout))
      return failAndRecover("tx_data_ready");
    // One block: bytes from txBuffer to DATA, one per DRE. SRCADDR is the end address
    // when the source increments. TX ring is never used; the RX ISR cannot write DATA.
    radioDmaDesc.BTCTRL.reg = DMAC_BTCTRL_VALID | DMAC_BTCTRL_BEATSIZE_BYTE | DMAC_BTCTRL_SRCINC |
                              DMAC_BTCTRL_BLOCKACT_NOACT;
    radioDmaDesc.BTCNT.reg = uint16_t(length + 2);
    radioDmaDesc.SRCADDR.reg = uint32_t(txBuffer) + length + 2;
    radioDmaDesc.DSTADDR.reg = uint32_t(&SERCOM0->USART.DATA.reg);
    radioDmaDesc.DESCADDR.reg = 0;
    __DSB();
    DMAC->CHID.reg = DMAC_CHID_ID(RADIO_DMA_CHANNEL);
    DMAC->CHINTFLAG.reg = DMAC_CHINTFLAG_TCMPL | DMAC_CHINTFLAG_TERR | DMAC_CHINTFLAG_SUSP;
    DMAC->CHCTRLA.reg = DMAC_CHCTRLA_ENABLE;
    // CHID is shared state; nothing else uses the DMAC, so it stays on our channel.
    if (!wait([] { DMAC->CHID.reg = DMAC_CHID_ID(RADIO_DMA_CHANNEL);
                   return (DMAC->CHINTFLAG.reg & (DMAC_CHINTFLAG_TCMPL | DMAC_CHINTFLAG_TERR)) != 0; },
              start, timeout) || DMAC->CHINTFLAG.bit.TERR) {
      dmaStop(); ++dmaTimeouts;
      return failAndRecover("tx_dma");
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
