// IMD board firmware (STM32G031K8, HV side). Pin map: DESIGN_NOTES.md "MCU pin map".
//
// Loop: ADS1220 round-robin VBUS / VE / VKE (single-shot, 90 SPS) -> imd::Monitor -> R0 switches,
// MHS PWM, OKHS / IMD_OK. Status line on USART (PA2/PA3, through the ISO7721) once a second.
// The independent watchdog resets the MCU if the loop stalls; reset releases every output low.
//
// Calibration (DESIGN_NOTES.md "Calibration") over the same UART, one command per line:
//   CAL               show the stored calibration
//   CAL GAIN <volts>  E and KE tied to HV+, <volts> = HV+ to HV- read on a meter
//   CAL BRIDGE <ohms> known resistor from HV+ to the floating E/KE node (k / M suffix allowed)
//   CAL ABORT         stop a running step
//   CAL CLEAR         forget the calibration (back to nominal values)
// While a step runs the board is NOT monitoring: MHS shows device error (40 Hz), OKHS/IMD_OK low.
#include <Arduino.h>
#include <SPI.h>
#include <IWatchdog.h>
#include <stdlib.h>
#include <string.h>
#include <EEPROM.h>   // STM32duino flash emulation; the buffered API below avoids one erase per byte
#include "ads1220.h"
#include "imd_cal.h"
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

// Sense divider: 4 x 1.2 MOhm over 18 kOhm (all three channels identical; calibration trims it).
constexpr float kDivGain = (4.8e6f + 18.0e3f) / 18.0e3f;
constexpr uint32_t kAdcTimeoutMs = 50;      // 90 SPS single-shot is ~11 ms
constexpr uint32_t kMhsTickHz = 100000;     // PWM timer tick: 10 Hz = 10000 ticks (16-bit OK)

enum class Mode : uint8_t { Run, CalGain, CalBridge };

static Ads1220 adc(SPI, pins::ADC_CS, pins::ADC_DRDY);
static imd::Calibration cal;
static imd::Monitor monitor;
static imd::GainCal gainCal;
static imd::BridgeCal bridgeCal;
static Mode mode = Mode::Run;
static HardwareTimer* mhsTimer = nullptr;
static imd::PwmCmd mhsNow;
static bool adcOk = false;

// ---------------------------------------------------------------- outputs

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

// Latched if anything ever asks for both R0 switches at once (the state machine never does).
static bool r0Interlock = false;

static void setR0(bool p, bool n) {
  // Interlock: never both on. Not dangerous (~0.45 mA through 2 x R0), but every reading taken
  // like that is meaningless, so it latches a device error until reset.
  if (p && n) {
    p = n = false;
    if (!r0Interlock) Serial.println("R0 INTERLOCK: both switches requested - device error until reset");
    r0Interlock = true;
  }
  // Break before make.
  if (!p) digitalWrite(pins::R0P_EN, LOW);
  if (!n) digitalWrite(pins::R0N_EN, LOW);
  if (p) digitalWrite(pins::R0P_EN, HIGH);
  if (n) digitalWrite(pins::R0N_EN, HIGH);
}

static void setOk(bool ok) {
  digitalWrite(pins::OKHS, ok ? HIGH : LOW);
  digitalWrite(pins::IMD_OK, ok ? HIGH : LOW);
}

// ---------------------------------------------------------------- calibration storage (flash)

static void loadCal() {
  eeprom_buffer_fill();
  uint8_t* p = reinterpret_cast<uint8_t*>(&cal);
  for (uint32_t i = 0; i < sizeof cal; i++) p[i] = eeprom_buffered_read_byte(i);
  if (!imd::calValid(cal)) cal = imd::Calibration();   // erased / corrupt: nominal values, flags 0
}

static void saveCal() {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&cal);
  for (uint32_t i = 0; i < sizeof cal; i++) eeprom_buffered_write_byte(i, p[i]);
  eeprom_buffer_flush();   // one page erase + write (~tens of ms, well inside the watchdog)
}

static void restartMonitor() { monitor = imd::Monitor(imd::applyCal(imd::Config(), cal)); }

// ---------------------------------------------------------------- serial helpers (integer printf only)

// Ohms -> "1234k" / "inf" for the status line.
static const char* kohm(float ohms, char* buf) {
  if (isinf(ohms) || ohms > 1e9f) return "inf";
  snprintf(buf, 12, "%luk", (unsigned long)(ohms / 1000.0f + 0.5f));
  return buf;
}

static long ppm(float gain) { return lroundf((gain - 1.0f) * 1e6f); }

static void showCal() {
  Serial.printf("CAL steps=%s%s gain_ppm vbus=%+ld ve=%+ld vke=%+ld r0=%lu rdiv=%lu %s\n",
                (cal.flags & imd::Calibration::kGain) ? "G" : "-",
                (cal.flags & imd::Calibration::kBridge) ? "B" : "-",
                ppm(cal.gVbus), ppm(cal.gVe), ppm(cal.gVke),
                (unsigned long)lroundf(cal.r0), (unsigned long)lroundf(cal.rdiv),
                cal.flags ? "" : "(nominal values: run CAL GAIN, then CAL BRIDGE)");
}

static void endCal(const imd::CalResult& r) {
  setR0(false, false);
  if (r.ok) saveCal();
  Serial.printf("CAL %s: %s\n", r.ok ? "OK" : "FAILED", r.msg);
  showCal();
  mode = Mode::Run;
  restartMonitor();
}

// Minimal decimal parser ("250", "250.3"): newlib's strtof costs ~20 kB of a 64 kB part.
// Returns the value and leaves *s after the number; 0 if there is no number.
static float parseNum(const char** s) {
  const char* p = *s;
  while (*p == ' ') p++;
  float v = 0.0f, scale = 0.0f;
  bool any = false;
  for (; *p; p++) {
    if (*p >= '0' && *p <= '9') {
      any = true;
      if (scale == 0.0f) {
        v = v * 10.0f + (float)(*p - '0');
      } else {
        v += (float)(*p - '0') * scale;
        scale *= 0.1f;
      }
    } else if (*p == '.' && scale == 0.0f) {
      scale = 0.1f;
    } else {
      break;
    }
  }
  *s = p;
  return any ? v : 0.0f;
}

// "1000000", "1000k", "1M", "1.002M" -> ohms; 0 on error.
static float parseOhms(const char* s) {
  float v = parseNum(&s);
  if (*s == 'k' || *s == 'K') v *= 1e3f;
  else if (*s == 'M') v *= 1e6f;
  return v;
}

static void command(char* line) {
  // Upper-case the command words only; the argument keeps its case ("k" vs "M" suffix).
  for (char* c = line; *c && !(*c >= '0' && *c <= '9'); c++)
    if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
  char* arg = strchr(line, ' ');
  if (strncmp(line, "CAL", 3) != 0) {
    Serial.println("? commands: CAL | CAL GAIN <V> | CAL BRIDGE <ohms> | CAL ABORT | CAL CLEAR");
    return;
  }
  while (arg && *arg == ' ') arg++;
  if (!arg || !*arg) {
    showCal();
  } else if (strncmp(arg, "GAIN", 4) == 0) {
    const char* p = arg + 4;
    const float v = parseNum(&p);
    gainCal.start(v);
    mode = Mode::CalGain;
    Serial.println("CAL GAIN running (~4 s): E and KE must be tied to HV+");
  } else if (strncmp(arg, "BRIDGE", 6) == 0) {
    char* p = arg + 6;
    while (*p == ' ') p++;
    bridgeCal.start(parseOhms(p), imd::applyCal(imd::Config(), cal));
    mode = Mode::CalBridge;
    Serial.println("CAL BRIDGE running (~10-30 s): Rcal from HV+ to E/KE, nothing else connected");
  } else if (strncmp(arg, "ABORT", 5) == 0) {
    if (mode != Mode::Run) {
      imd::CalResult r;
      r.msg = "aborted";
      endCal(r);
    }
  } else if (strncmp(arg, "CLEAR", 5) == 0) {
    cal = imd::Calibration();
    imd::calSeal(&cal);
    saveCal();
    restartMonitor();
    Serial.println("CAL cleared");
    showCal();
  } else {
    Serial.println("? CAL GAIN <V> | CAL BRIDGE <ohms> | CAL ABORT | CAL CLEAR");
  }
}

static void pollSerial() {
  static char buf[48];
  static uint8_t n = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (n) {
        buf[n] = 0;
        command(buf);
        n = 0;
      }
    } else if (n < sizeof buf - 1) {
      buf[n++] = c;
    }
  }
}

// ---------------------------------------------------------------- setup / loop

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
  setOk(false);
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
  loadCal();
  restartMonitor();
  SPI.setMOSI(pins::ADC_MOSI);
  SPI.setMISO(pins::ADC_MISO);
  SPI.setSCLK(pins::ADC_SCK);
  SPI.begin();
  adcOk = adc.begin();
  Serial.printf("IMD fw start, ADS1220 %s\n", adcOk ? "ok" : "NOT RESPONDING");
  showCal();

  IWatchdog.begin(1000000);  // 1 s
}

void loop() {
  IWatchdog.reload();
  pollSerial();

  // ---- ADC round robin: one sample set = VBUS, VE, VKE, VBUS (~45 ms). VBUS is read before and
  // after the chassis channels: their mean is VBUS at the moment of VE/VKE, and their difference
  // tells the monitor that the pack voltage moved during the set.
  static const Ads1220::Channel order[4] = {Ads1220::AIN0, Ads1220::AIN1, Ads1220::AIN2, Ads1220::AIN0};
  static float v[4];
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
      if (++idx == 4) {
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
    const float vbusRaw = 0.5f * (v[0] + v[3]);
    in.vbus = adcFault ? 0.0f : vbusRaw * cal.gVbus;  // vbus 0 -> device error inside the monitor
    in.vbusDelta = fabsf(v[3] - v[0]) * cal.gVbus;
    in.ve = v[1] * cal.gVe;
    in.vke = v[2] * cal.gVke;
    in.viso = readViso();
    in.cont1 = digitalRead(pins::CONT1);
    in.cont2 = digitalRead(pins::CONT2);
    in.inhibit = digitalRead(pins::INHIBIT);

    if (mode == Mode::Run) {
      const imd::Outputs out = monitor.step(millis(), in);
      setR0(out.r0p, out.r0n);
      if (r0Interlock) {
        setR0(false, false);
        applyMhs(imd::encodeMhs(imd::MhsMode::DeviceError, 0));
        setOk(false);
      } else {
        applyMhs(out.mhs);
        setOk(out.okhs);
      }
    } else {
      // Calibrating: not monitoring, so report a device error and never "OK".
      applyMhs(imd::encodeMhs(imd::MhsMode::DeviceError, 0));
      setOk(false);
      if (adcFault) {
        imd::CalResult r;
        r.msg = "ADC fault";
        endCal(r);
      } else if (mode == Mode::CalGain) {
        setR0(false, false);
        if (gainCal.add(vbusRaw, v[1], v[2])) endCal(gainCal.finish(&cal));   // raw: before gains
      } else {
        const imd::Outputs out = bridgeCal.step(millis(), in);              // with current gains
        setR0(out.r0p, out.r0n);
        if (bridgeCal.done()) endCal(bridgeCal.finish(&cal));
      }
    }
  }

  // ---- Status line and heartbeat, once a second.
  static uint32_t tLog = 0;
  static char bufP[12], bufN[12];
  if (millis() - tLog >= 1000) {
    tLog = millis();
    digitalWrite(pins::LED_HB, !digitalRead(pins::LED_HB));
    if (mode != Mode::Run) {
      Serial.printf("CAL %s running, Vbus=%ld\n", mode == Mode::CalGain ? "GAIN" : "BRIDGE",
                    (long)(v[0] * cal.gVbus));
      return;
    }
    const imd::Status& s = monitor.status();
    // state: ok | DEGRADED (no valid reading for 60 s, last one held) | INTERLOCK
    const char* state = r0Interlock ? "INTERLOCK" : s.stale ? "DEGRADED" : "ok";
    Serial.printf("IMD mode=%s ok=%d state=%s age=%lus Rp=%s Rn=%s Vbus=%ld.%01ld fP=%ldppm fN=%ldppm scope=%s "
                  "cyc=%lu settled=%d relaxed=%d restarts=%lu err=%u/%u cal=%s%s\n",
                  imd::mhsModeName(s.mode), (int)(s.mode == imd::MhsMode::Normal && s.insOk && !r0Interlock),
                  state, (unsigned long)(s.ageMs / 1000),
                  kohm(s.rp, bufP), kohm(s.rn, bufN), (long)s.vbus, labs((long)(s.vbus * 10.0f)) % 10,
                  (long)(s.fP * 1e6f), (long)(s.fN * 1e6f),
                  s.systemScope ? "system" : "pack", (unsigned long)s.cycles, (int)s.lastSettled,
                  (int)s.lastRelaxed, (unsigned long)s.transientRestarts,
                  s.badCount, s.earthCount,
                  (cal.flags & imd::Calibration::kGain) ? "G" : "-",
                  (cal.flags & imd::Calibration::kBridge) ? "B" : "-");
  }
}
