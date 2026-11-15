// nRF52840 Pro Micro bring-up probe (no sensors attached).
// Commands over USB serial (one per line):
//   i      board, bootloader, SoftDevice, FICR/UICR, regulator and USB state
//   c      LF clock: measure LFRC, try the 32.768 kHz crystal (with timeout), measure it
//   a      ADC: VDD, VDDH/5, AIN0 (P0.02), AIN2 (P0.04), AIN5 (P0.29), AIN7 (P0.31)
//   u      RYLR998 UART timing: 250 bytes via Serial1 (64-byte chunks) vs one DMA transfer
//   p      reed-path loopback on P0.22: generated pulses, bounce and dropouts -> PulseCounter
//   v0/v1  drive P0.13 (VCC switch on nice!nano-style boards) low/high, for a meter check
//   dfu/uf2 reset into the bootloader (serial DFU only / UF2 drive + serial)
//   all    i, c, a, u, p
#include <Adafruit_TinyUSB.h>
#include <nrf_sdm.h>
#include "Timebase52.h"

struct Snap { uint64_t raw, count, revs, period, a, b; };
constexpr uint8_t PIN_WIND = 22, PIN_VCC_SWITCH = 13, PIN_LED = 15;

// ---- cycle counter (64 MHz CPU clock) ----
static void cycBegin() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
static inline uint32_t cyc() { return DWT->CYCCNT; }
static void busyUs(uint32_t us) { uint32_t t = cyc(); while (cyc() - t < us * 64u) {} }

static bool hfxoStart() {
  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_HFCLKSTART = 1;
  uint32_t t = cyc();
  while (!NRF_CLOCK->EVENTS_HFCLKSTARTED) if (cyc() - t > 64000000u) return false;
  return true;
}

// ---- info ----
static void info() {
  Serial.println("== board ==");
  Serial.printf("bootloader version 0x%08lx, reset reason 0x%08lx\n", (unsigned long)bootloaderVersion,
                (unsigned long)readResetReason());
  const uint32_t sdBase = 0x1000;
  const uint32_t magic = *(const uint32_t *)(sdBase + SOFTDEVICE_INFO_STRUCT_OFFSET + 4);
  if (magic == 0x51B1E5DBu)
    Serial.printf("SoftDevice: fwid 0x%04x, version %lu (6.1.1 = S140 fwid 0x00B6)\n", SD_FWID_GET(sdBase),
                  (unsigned long)SD_VERSION_GET(sdBase));
  else
    Serial.printf("SoftDevice: none (magic 0x%08lx)\n", (unsigned long)magic);
  const uint32_t v = NRF_FICR->INFO.VARIANT;
  Serial.printf("FICR part %lx variant %c%c%c%c package %lx RAM %lu KB flash %lu KB\n",
                (unsigned long)NRF_FICR->INFO.PART, char(v >> 24), char(v >> 16), char(v >> 8), char(v),
                (unsigned long)NRF_FICR->INFO.PACKAGE, (unsigned long)NRF_FICR->INFO.RAM,
                (unsigned long)NRF_FICR->INFO.FLASH);
  Serial.printf("device id %08lx%08lx, BLE addr %04lx%08lx\n", (unsigned long)NRF_FICR->DEVICEID[1],
                (unsigned long)NRF_FICR->DEVICEID[0], (unsigned long)(NRF_FICR->DEVICEADDR[1] & 0xffff),
                (unsigned long)NRF_FICR->DEVICEADDR[0]);
  const uint32_t regout = NRF_UICR->REGOUT0 & 7;
  static const char *vout[] = {"1.8", "2.1", "2.4", "2.7", "3.0", "3.3", "?", "1.8 (default)"};
  Serial.printf("UICR NFCPINS 0x%08lx (%s), REGOUT0 VOUT %lu = %s V, APPROTECT 0x%08lx, PSELRESET %08lx\n",
                (unsigned long)NRF_UICR->NFCPINS, (NRF_UICR->NFCPINS & 1) ? "P0.09/P0.10 are NFC" : "P0.09/P0.10 are GPIO",
                (unsigned long)regout, vout[regout], (unsigned long)NRF_UICR->APPROTECT,
                (unsigned long)NRF_UICR->PSELRESET[0]);
  Serial.printf("POWER MAINREGSTATUS %lu (%s), DCDCEN %lu, DCDCEN0 %lu, USBREGSTATUS 0x%lx (VBUS %s)\n",
                (unsigned long)NRF_POWER->MAINREGSTATUS,
                NRF_POWER->MAINREGSTATUS ? "high-voltage mode: supply on VDDH" : "normal mode: supply on VDD",
                (unsigned long)NRF_POWER->DCDCEN, (unsigned long)NRF_POWER->DCDCEN0,
                (unsigned long)NRF_POWER->USBREGSTATUS, (NRF_POWER->USBREGSTATUS & 1) ? "present" : "absent");
  Serial.printf("USB mounted %d, LFCLKSTAT 0x%08lx, HFCLKSTAT 0x%08lx\n", TinyUSBDevice.mounted(),
                (unsigned long)NRF_CLOCK->LFCLKSTAT, (unsigned long)NRF_CLOCK->HFCLKSTAT);
}

// ---- LF clock ----
static double measureLf(uint32_t nTicks) {
  // CPU cycles (HFXO-locked) per nTicks of RTC2 -> LF frequency in Hz.
  uint32_t c0 = NRF_RTC2->COUNTER, start = cyc();
  while (NRF_RTC2->COUNTER == c0) if (cyc() - start > 64000000u) return 0;
  uint32_t t0 = cyc(), r0 = NRF_RTC2->COUNTER;
  while (((NRF_RTC2->COUNTER - r0) & 0xffffff) < nTicks) if (cyc() - t0 > 128000000u) return 0;
  uint32_t dt = cyc() - t0;
  return double(nTicks) * 64e6 / double(dt);
}

static bool lfStart(uint32_t src, uint32_t timeoutCycles) {
  NRF_CLOCK->TASKS_LFCLKSTOP = 1;
  uint32_t t = cyc();
  while (NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_STATE_Msk) if (cyc() - t > 6400000u) break;
  NRF_CLOCK->LFCLKSRC = src << CLOCK_LFCLKSRC_SRC_Pos;
  NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_LFCLKSTART = 1;
  t = cyc();
  while (!NRF_CLOCK->EVENTS_LFCLKSTARTED) if (cyc() - t > timeoutCycles) return false;
  return true;
}

static void clockTest() {
  Serial.println("== LF clock ==");
  Serial.printf("HFXO start: %s\n", hfxoStart() ? "ok (CPU timing now crystal-accurate, ~+-40 ppm)" : "FAILED");
  const uint32_t stat = NRF_CLOCK->LFCLKSTAT;
  Serial.printf("LFCLKSTAT before 0x%08lx (src %lu: 0 = RC, 1 = Xtal, 2 = Synth)\n", (unsigned long)stat,
                (unsigned long)(stat & 3));
  if (lfStart(CLOCK_LFCLKSRC_SRC_RC, 64000000u)) {
    double f = measureLf(32768);
    Serial.printf("LFRC: %.2f Hz (%+.0f ppm)\n", f, (f / 32768.0 - 1) * 1e6);
  } else Serial.println("LFRC: failed to start");
  const bool xtal = lfStart(CLOCK_LFCLKSRC_SRC_Xtal, 2 * 64000000u);
  if (xtal) {
    delay(300);  // let the crystal amplitude settle
    double f = measureLf(65536);
    Serial.printf("LFXO: started, %.3f Hz (%+.1f ppm), LFCLKSTAT 0x%08lx -> keeping the crystal\n", f,
                  (f / 32768.0 - 1) * 1e6, (unsigned long)NRF_CLOCK->LFCLKSTAT);
  } else {
    Serial.println("LFXO: no start within 2 s (no crystal fitted?) -> back to LFRC");
    lfStart(CLOCK_LFCLKSRC_SRC_RC, 64000000u);
  }
}

// ---- ADC ----
static int16_t saadc(uint32_t pselp) {
  static volatile int16_t result;
  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
  NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Over16x;  // also needs no BURST: done as 16 samples
  NRF_SAADC->CH[0].PSELP = pselp;
  NRF_SAADC->CH[0].PSELN = 0;
  NRF_SAADC->CH[0].CONFIG = (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
                            (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
                            (SAADC_CH_CONFIG_TACQ_40us << SAADC_CH_CONFIG_TACQ_Pos) |
                            (SAADC_CH_CONFIG_BURST_Enabled << SAADC_CH_CONFIG_BURST_Pos);
  for (int i = 1; i < 8; ++i) NRF_SAADC->CH[i].PSELP = 0;
  NRF_SAADC->RESULT.PTR = (uint32_t)&result;
  NRF_SAADC->RESULT.MAXCNT = 1;
  NRF_SAADC->ENABLE = 1;
  NRF_SAADC->EVENTS_STARTED = NRF_SAADC->EVENTS_END = NRF_SAADC->EVENTS_STOPPED = 0;
  NRF_SAADC->TASKS_START = 1;
  while (!NRF_SAADC->EVENTS_STARTED) {}
  NRF_SAADC->TASKS_SAMPLE = 1;
  while (!NRF_SAADC->EVENTS_END) {}
  NRF_SAADC->TASKS_STOP = 1;
  while (!NRF_SAADC->EVENTS_STOPPED) {}
  NRF_SAADC->ENABLE = 0;
  return result;
}

static void adcTest() {
  Serial.println("== ADC (gain 1/6, 0.6 V ref: full scale 3.6 V; 16x oversampled) ==");
  NRF_SAADC->ENABLE = 1;
  NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
  NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
  while (!NRF_SAADC->EVENTS_CALIBRATEDONE) {}
  NRF_SAADC->ENABLE = 0;
  struct { const char *name; uint32_t sel; int mult; } ch[] = {
      {"VDD", SAADC_CH_PSELP_PSELP_VDD, 1},          {"VDDH/5 (x5)", SAADC_CH_PSELP_PSELP_VDDHDIV5, 5},
      {"AIN0 P0.02", SAADC_CH_PSELP_PSELP_AnalogInput0, 1}, {"AIN2 P0.04", SAADC_CH_PSELP_PSELP_AnalogInput2, 1},
      {"AIN5 P0.29", SAADC_CH_PSELP_PSELP_AnalogInput5, 1}, {"AIN7 P0.31", SAADC_CH_PSELP_PSELP_AnalogInput7, 1}};
  for (auto &c : ch) {
    int lo = 99999, hi = -99999; long sum = 0;
    for (int i = 0; i < 10; ++i) { int r = saadc(c.sel); sum += r; lo = min(lo, r); hi = max(hi, r); }
    const double mv = sum / 10.0 * 3600.0 / 4096.0 * c.mult;
    Serial.printf("%-12s %7.0f mV  (raw %d..%d)\n", c.name, mv, lo, hi);
  }
}

// ---- UART timing ----
static uint8_t txbuf[256];
static void uartTest() {
  Serial.println("== UART TX timing, 250 bytes at 115200 (ideal 21.70 ms; nRF actual baud 115108 -> 21.72 ms) ==");
  for (int i = 0; i < 250; ++i) txbuf[i] = 'A' + i % 26;
  Serial1.begin(115200);
  delay(20);
  for (int rep = 0; rep < 3; ++rep) {
    uint32_t t0 = cyc();
    Serial1.write(txbuf, 250);
    Serial1.flush();
    uint32_t t1 = cyc();
    // One DMA transfer of the whole buffer, core IRQ masked.
    NVIC_DisableIRQ(UARTE0_UART0_IRQn);
    NRF_UARTE0->EVENTS_ENDTX = 0;
    NRF_UARTE0->TXD.PTR = (uint32_t)txbuf;
    NRF_UARTE0->TXD.MAXCNT = 250;
    uint32_t t2 = cyc();
    NRF_UARTE0->TASKS_STARTTX = 1;
    while (!NRF_UARTE0->EVENTS_ENDTX) {}
    uint32_t t3 = cyc();
    NRF_UARTE0->EVENTS_ENDTX = 0;
    NVIC_ClearPendingIRQ(UARTE0_UART0_IRQn);
    NVIC_EnableIRQ(UARTE0_UART0_IRQn);
    Serial.printf("Serial1 (64-byte chunks): %.3f ms   single DMA: %.3f ms\n", (t1 - t0) / 64e3, (t3 - t2) / 64e3);
  }
  Serial.println("(ENDTX fires when the last byte has left the DMA, ~1 byte before it leaves the pin)");
  Serial1.end();
}

// ---- reed-path loopback ----
static void level(bool high) {
  if (high) NRF_P0->OUTSET = 1u << PIN_WIND; else NRF_P0->OUTCLR = 1u << PIN_WIND;
}
static Snap snap() {
  __disable_irq();
  Snap s{tb::rawEdges, tb::pulses.count, tb::pulses.revolutions, tb::pulses.revolutionPeriod,
         tb::pulses.intervalA, tb::pulses.intervalB};
  __enable_irq();
  return s;
}
// One closure: optional bounce at make and break, optional mid-closure dropout.
static void closure(uint32_t lowMs, int bounces, uint32_t dropoutMs) {
  for (int i = 0; i < bounces; ++i) { level(false); busyUs(300); level(true); busyUs(300); }
  level(false);
  if (dropoutMs) {
    busyUs((lowMs / 2) * 1000);
    level(true); busyUs(dropoutMs * 1000); level(false);
    busyUs((lowMs - lowMs / 2) * 1000);
  } else busyUs(lowMs * 1000);
  for (int i = 0; i < bounces; ++i) { level(true); busyUs(300); level(false); busyUs(300); }
  level(true);
}
static void run(const char *name, int closures, uint32_t spacingMs, uint32_t lowMs, int bounces, uint32_t dropoutMs) {
  Snap s0 = snap();
  uint32_t t = cyc();
  for (int i = 0; i < closures; ++i) {
    while (cyc() - t < spacingMs * 64000u) {}
    t += spacingMs * 64000u;
    closure(lowMs, bounces, dropoutMs);
  }
  delay(50);  // let the last debounce settle
  Snap s1 = snap();
  const uint64_t expectPeriod = uint64_t(2) * spacingMs * tb::HZ / 1000;
  Serial.printf("%-34s raw edges %4llu  pulses %3llu  revs %3llu  period %6llu (A %llu, B %llu) expect %llu\n", name,
                s1.raw - s0.raw, s1.count - s0.count, s1.revs - s0.revs, s1.period, s1.a, s1.b, expectPeriod);
}
static void pulseTest() {
  Serial.println("== reed path loopback on P0.22 (SENSE -> PPI -> EGU3, 10 ms debounce on RTC2) ==");
  run("clean, 2 rev/s, 40 ms closures", 40, 250, 40, 0, 0);
  run("bounce 3x0.3 ms at make/break", 40, 250, 40, 3, 0);
  run("15 ms dropout mid-closure (60 ms)", 20, 250, 60, 0, 15);
  run("5 ms dropout mid-closure (60 ms)", 20, 250, 60, 0, 5);
  run("fast, 10 rev/s, 20 ms closures", 100, 50, 20, 0, 0);
  Serial.println("expect: clean/bounce/5 ms -> pulses = closures; 15 ms dropout -> 2 pulses per closure");
}

void setup() {
  cycBegin();
  pinMode(PIN_LED, OUTPUT);
  Serial.begin(115200);
  tb::begin();
  tb::edgeBegin(PIN_WIND, true);
}

static String line;
void loop() {
  static uint32_t blink = 0;
  if (millis() - blink > 1000) { blink = millis(); digitalWrite(PIN_LED, HIGH); delay(5); digitalWrite(PIN_LED, LOW); }
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch != '\n' && ch != '\r') { line += ch; continue; }
    line.trim();
    if (line == "i" || line == "all") info();
    if (line == "c" || line == "all") clockTest();
    if (line == "a" || line == "all") adcTest();
    if (line == "u" || line == "all") uartTest();
    if (line == "p" || line == "all") pulseTest();
    if (line == "v0" || line == "v1") {
      pinMode(PIN_VCC_SWITCH, OUTPUT);
      digitalWrite(PIN_VCC_SWITCH, line == "v1");
      Serial.printf("P0.13 driven %s: measure the VCC pin now\n", line == "v1" ? "HIGH" : "LOW");
    }
    if (line == "dfu" || line == "uf2") {
      // Reset into the bootloader: 0x4E = serial DFU only, 0x57 = UF2 drive + serial.
      Serial.printf("resetting to bootloader (%s)\n", line.c_str());
      Serial.flush(); delay(100);
      NRF_POWER->GPREGRET = line == "dfu" ? 0x4E : 0x57;
      NVIC_SystemReset();
    }
    if (line.length()) Serial.println("== done ==");
    line = "";
  }
  delay(10);
}
