#include "ads1220.h"

namespace {
constexpr uint8_t CMD_RESET = 0x06;  // 0000 011x
constexpr uint8_t CMD_START = 0x08;  // 0000 100x
constexpr uint8_t CMD_RDATA = 0x10;  // 0001 xxxx
constexpr uint8_t CMD_RREG = 0x20;   // 0010 rrnn
constexpr uint8_t CMD_WREG = 0x40;   // 0100 rrnn

// Reg 1: DR = 010b (90 SPS normal mode), MODE = 00 normal, CM = 0 single-shot, TS = 0, BCS = 0.
constexpr uint8_t REG1 = 0x40;
// Reg 2: VREF = 00 internal 2.048 V, 50/60 = 00 (only valid at 20 SPS), PSW = 0, IDAC = off.
constexpr uint8_t REG2 = 0x00;
// Reg 3: IDAC muxes off, DRDYM = 0 (DRDY on the dedicated pin only).
constexpr uint8_t REG3 = 0x00;
// Reg 0 low nibble: GAIN = 000 (x1), PGA_BYPASS = 1 (required for AINN = AVSS).
constexpr uint8_t REG0_LOW = 0x01;
}  // namespace

void Ads1220::command(uint8_t c) {
  spi_.beginTransaction(settings_);
  digitalWrite(cs_, LOW);
  spi_.transfer(c);
  digitalWrite(cs_, HIGH);
  spi_.endTransaction();
}

void Ads1220::writeReg(uint8_t reg, uint8_t val) {
  spi_.beginTransaction(settings_);
  digitalWrite(cs_, LOW);
  spi_.transfer(CMD_WREG | (uint8_t)(reg << 2));  // nn = 0: one register
  spi_.transfer(val);
  digitalWrite(cs_, HIGH);
  spi_.endTransaction();
}

uint8_t Ads1220::readReg(uint8_t reg) {
  spi_.beginTransaction(settings_);
  digitalWrite(cs_, LOW);
  spi_.transfer(CMD_RREG | (uint8_t)(reg << 2));
  uint8_t v = spi_.transfer(0xFF);
  digitalWrite(cs_, HIGH);
  spi_.endTransaction();
  return v;
}

bool Ads1220::begin() {
  pinMode(cs_, OUTPUT);
  digitalWrite(cs_, HIGH);
  pinMode(drdy_, INPUT_PULLUP);
  command(CMD_RESET);
  delay(1);  // reset takes 50 us + 32 tCLK
  writeReg(0, (uint8_t)(AIN0 << 4) | REG0_LOW);
  writeReg(1, REG1);
  writeReg(2, REG2);
  writeReg(3, REG3);
  return readReg(1) == REG1 && readReg(0) == ((uint8_t)(AIN0 << 4) | REG0_LOW);
}

void Ads1220::start(Channel ch) {
  writeReg(0, (uint8_t)(ch << 4) | REG0_LOW);
  command(CMD_START);
}

float Ads1220::readVolts() {
  spi_.beginTransaction(settings_);
  digitalWrite(cs_, LOW);
  spi_.transfer(CMD_RDATA);
  uint32_t raw = (uint32_t)spi_.transfer(0xFF) << 16;
  raw |= (uint32_t)spi_.transfer(0xFF) << 8;
  raw |= spi_.transfer(0xFF);
  digitalWrite(cs_, HIGH);
  spi_.endTransaction();
  int32_t code = (raw & 0x800000) ? (int32_t)(raw | 0xFF000000u) : (int32_t)raw;  // 24-bit two's complement
  return (float)code * (kVref / 8388608.0f);
}
