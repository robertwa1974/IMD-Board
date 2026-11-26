// IMD board firmware (STM32G031K8, HV side). Pin map: DESIGN_NOTES.md "MCU pin map".
//
// Loop: ADS1220 round-robin VBUS / VE / VKE (single-shot, 90 SPS) -> imd::Monitor -> R0 switches,
// MHS PWM, OKHS / IMD_OK. Status line on USART (PA2/PA3, through the ISO7721) once a second.
// The independent watchdog resets the MCU if the loop stalls; reset releases every output low.
#include <Arduino.h>
#include <SPI.h>
#include <IWatchdog.h>
#include "ads1220.h"
#include "imd_monitor.h"

namespace pins {
constexpr uint32_t VISO_MON = PA0;
constexpr uint32_t ADC_CS = PA4, ADC_SCK = PA5, ADC_MISO = PA6, ADC_MOSI = PA7, ADC_DRDY = PB0;
constexpr uint32_t R0P_EN = PB1, R0N_EN = PB2;
constexpr uint32_t MHS = PA8;  // TIM1_CH1
constexpr uint32_t OKHS = PA11, IMD_OK = PA12;
constexpr uint32_t CONT1 = PB4, CONT2 = PB5, INHIBIT = PB6;
constexpr uint32_t LED_HB = PB7;
}  // namespace pins

// Sense divider: 4 x 1.2 MOhm over 18 kOhm (all three channels identical).
constexpr float kDivGain = (4.8e6f + 18.0e3f) / 18.0e3f;
constexpr uint32_t kAdcTimeoutMs = 50;      // 90 SPS single-shot is ~11 ms
constexpr uint32_t kMhsTickHz = 100000;     // PWM timer tick: 10 Hz = 10000 ticks (16-bit OK)

static Ads1220 adc(SPI, pins::ADC_CS, pins::ADC_DRDY);
static imd::Monitor monitor;
static HardwareTimer* mhsTimer = nullptr;
static imd::PwmCmd mhsNow;
static bool adcOk = false;

static void applyMhs(const imd::PwmCmd& c) {
  if (c == mhsNow) return;
  mhsNow = c;
  if (c.freqHz == 0) {
    mhsTimer->setCaptureCompare(1, 0, TICK_COMPARE_FORMAT);  // PWM1 with CCR = 0: constantly low
    return;
  }
  const uint32_t period = kMhsTickHz / c.freqHz;
  mhsTimer->setOverflow(period, TICK_FORMAT);
  mhsTimer->setCaptureCompare(1, (uint32_t)(period * c.dutyPct / 100.0f + 0.5f), TICK_COMPARE_FORMAT);
}

static void setR0(bool p, bool n) {
  // Break before make: never both on (harmless, ~0.45 mA, but it would corrupt the sample).
  if (!p) digitalWrite(pins::R0P_EN, LOW);
  if (!n) digitalWrite(pins::R0N_EN, LOW);
  if (p) digitalWrite(pins::R0P_EN, HIGH);
  if (n) digitalWrite(pins::R0N_EN, HIGH);
}

// Ohms -> "1234k" / "inf" for the status line.
static const char* kohm(float ohms, char* buf) {
  if (isinf(ohms) || ohms > 1e9f) return "inf";
  snprintf(buf, 12, "%luk", (unsigned long)(ohms / 1000.0f + 0.5f));
  return buf;
}

static float readViso() {
  return analogRead(pins::VISO_MON) * (3.3f / 4095.0f) * 2.0f;  // 10k/10k divider
}

void setup() {
  // Everything off first: switches open, outputs "not OK".
  pinMode(pins::R0P_EN, OUTPUT);
  pinMode(pins::R0N_EN, OUTPUT);
  setR0(false, false);
  pinMode(pins::OKHS, OUTPUT);
  pinMode(pins::IMD_OK, OUTPUT);
  digitalWrite(pins::OKHS, LOW);
  digitalWrite(pins::IMD_OK, LOW);
  pinMode(pins::LED_HB, OUTPUT);
  pinMode(pins::CONT1, INPUT);
  pinMode(pins::CONT2, INPUT);
  pinMode(pins::INHIBIT, INPUT);
  analogReadResolution(12);

  mhsTimer = new HardwareTimer(TIM1);
  mhsTimer->setPrescaleFactor(mhsTimer->getTimerClkFreq() / kMhsTickHz);
  mhsTimer->setMode(1, TIMER_OUTPUT_COMPARE_PWM1, pins::MHS);
  mhsTimer->setOverflow(kMhsTickHz / 10, TICK_FORMAT);
  mhsTimer->setCaptureCompare(1, 0, TICK_COMPARE_FORMAT);
  mhsTimer->resume();

  Serial.begin(115200);
  SPI.setMOSI(pins::ADC_MOSI);
  SPI.setMISO(pins::ADC_MISO);
  SPI.setSCLK(pins::ADC_SCK);
  SPI.begin();
  adcOk = adc.begin();
  Serial.printf("IMD fw start, ADS1220 %s\n", adcOk ? "ok" : "NOT RESPONDING");

  IWatchdog.begin(1000000);  // 1 s
}

void loop() {
  IWatchdog.reload();

  // ---- ADC round robin: one sample set = VBUS, VE, VKE.
  static const Ads1220::Channel order[3] = {Ads1220::AIN0, Ads1220::AIN1, Ads1220::AIN2};
  static float v[3];
  static uint8_t idx = 0;
  static bool busy = false;
  static uint32_t tStart = 0;
  bool setDone = false;
  bool adcFault = !adcOk;

  if (adcOk) {
    if (!busy) {
      adc.start(order[idx]);
      tStart = millis();
      busy = true;
    } else if (adc.ready()) {
      v[idx] = adc.readVolts() * kDivGain;
      busy = false;
      if (++idx == 3) {
        idx = 0;
        setDone = true;
      }
    } else if (millis() - tStart > kAdcTimeoutMs) {
      busy = false;
      idx = 0;
      adcFault = true;
    }
  }

  if (setDone || adcFault) {
    imd::Inputs in;
    in.vbus = adcFault ? 0.0f : v[0];  // vbus 0 -> device error inside the monitor
    in.ve = v[1];
    in.vke = v[2];
    in.viso = readViso();
    in.cont1 = digitalRead(pins::CONT1);
    in.cont2 = digitalRead(pins::CONT2);
    in.inhibit = digitalRead(pins::INHIBIT);
    const imd::Outputs out = monitor.step(millis(), in);
    setR0(out.r0p, out.r0n);
    applyMhs(out.mhs);
    digitalWrite(pins::OKHS, out.okhs ? HIGH : LOW);
    digitalWrite(pins::IMD_OK, out.okhs ? HIGH : LOW);
  }

  // ---- Status line and heartbeat, once a second.
  static uint32_t tLog = 0;
  static char bufP[12], bufN[12];
  if (millis() - tLog >= 1000) {
    tLog = millis();
    digitalWrite(pins::LED_HB, !digitalRead(pins::LED_HB));
    const imd::Status& s = monitor.status();
    // Integer formatting only: float printf costs ~10 kB of a 64 kB part.
    Serial.printf("IMD mode=%s ok=%d Rp=%s Rn=%s Vbus=%ld.%01ld fP=%ldppm fN=%ldppm scope=%s cyc=%lu settled=%d err=%u/%u\n",
                  imd::mhsModeName(s.mode), (int)(s.mode == imd::MhsMode::Normal && s.insOk),
                  kohm(s.rp, bufP), kohm(s.rn, bufN), (long)s.vbus, (long)(s.vbus * 10.0f) % 10,
                  (long)(s.fP * 1e6f), (long)(s.fN * 1e6f),
                  s.systemScope ? "system" : "pack", (unsigned long)s.cycles, (int)s.lastSettled,
                  s.badCount, s.earthCount);
  }
}
