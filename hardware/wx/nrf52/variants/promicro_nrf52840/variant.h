// Pro Micro nRF52840 (nice!nano v2 / SuperMini-style clone). Arduino pin N is nRF GPIO N,
// so P0.xx = xx and P1.xx = 32 + xx: the silkscreen numbers can be used directly.
#ifndef _VARIANT_PROMICRO_NRF52840_WX_
#define _VARIANT_PROMICRO_NRF52840_WX_

#define VARIANT_MCK (64000000ul)
// Start on the internal RC so the RTOS tick runs even if no 32 kHz crystal is fitted;
// the application tries the crystal itself (with a timeout) once it is running.
#define USE_LFRC

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PINS_COUNT        (48)
#define NUM_DIGITAL_PINS  (48)
#define NUM_ANALOG_INPUTS (4)
#define NUM_ANALOG_OUTPUTS (0)

// On-board blue LED (P0.15) and the VCC (3.3 V out) switch (P0.13) on nice!nano-style boards.
#define PIN_LED1      (15)
#define LED_BUILTIN   PIN_LED1
#define LED_CONN      PIN_LED1
#define LED_BLUE      PIN_LED1
#define LED_RED       PIN_LED1
#define LED_STATE_ON  1
#define PIN_VCC_EN    (13)

// The exposed analog-capable pins.
#define PIN_A0 (2)   // P0.02 / AIN0
#define PIN_A1 (29)  // P0.29 / AIN5
#define PIN_A2 (31)  // P0.31 / AIN7
#define PIN_A3 (4)   // P0.04 / AIN2: battery divider on some boards (not on the header)
static const uint8_t A0 = PIN_A0;
static const uint8_t A1 = PIN_A1;
static const uint8_t A2 = PIN_A2;
static const uint8_t A3 = PIN_A3;
#define ADC_RESOLUTION 14
#define PIN_NFC1 (9)
#define PIN_NFC2 (10)

// Serial1 -> RYLR998.
#define PIN_SERIAL1_RX (8)   // P0.08
#define PIN_SERIAL1_TX (6)   // P0.06

// SPI is not used; spare header pins.
#define SPI_INTERFACES_COUNT 1
#define PIN_SPI_MISO (43)    // P1.11
#define PIN_SPI_MOSI (36)    // P1.04
#define PIN_SPI_SCK  (38)    // P1.06
static const uint8_t SS   = (11);
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK  = PIN_SPI_SCK;

// I2C: SHT31 + BMP180.
#define WIRE_INTERFACES_COUNT 1
#define PIN_WIRE_SDA (47)    // P1.15
#define PIN_WIRE_SCL (45)    // P1.13

#ifdef __cplusplus
}
#endif
#endif
