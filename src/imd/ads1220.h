// Minimal ADS1220 driver: single-shot, single-ended vs AVSS, PGA bypassed, internal 2.048 V ref.
// Command bytes, register fields and SPI mode from TI SBAS501D (sections 8.5.3, 8.6.2).
#pragma once
#include <Arduino.h>
#include <SPI.h>

class Ads1220 {
 public:
  // Single-ended channels (MUX[3:0] = 1000b + ch). Monitor channels per Table 8-10.
  enum Channel : uint8_t {
    AIN0 = 0x8, AIN1 = 0x9, AIN2 = 0xA, AIN3 = 0xB,
    REF_MON = 0xC,   // (VREFP - VREFN) / 4
    AVDD_MON = 0xD,  // (AVDD - AVSS) / 4
  };

  Ads1220(SPIClass& spi, uint8_t csPin, uint8_t drdyPin) : spi_(spi), cs_(csPin), drdy_(drdyPin) {}

  // Resets and configures. Returns false if the registers do not read back.
  bool begin();

  // Selects a channel and starts one conversion (non-blocking).
  void start(Channel ch);
  // True once DRDY has gone low after start().
  bool ready() const { return digitalRead(drdy_) == LOW; }
  // Reads the finished conversion, in volts at the ADC pin.
  float readVolts();

  static constexpr float kVref = 2.048f;

 private:
  void command(uint8_t c);
  void writeReg(uint8_t reg, uint8_t val);
  uint8_t readReg(uint8_t reg);

  SPIClass& spi_;
  uint8_t cs_, drdy_;
  // Datasheet: >= 160 kHz SCLK is fine; keep it modest for the long isolated-side traces.
  SPISettings settings_{1000000, MSBFIRST, SPI_MODE1};
};
