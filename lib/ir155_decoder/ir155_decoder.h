// IR155 MHS output decoder (Bender ISOMETER IR155-3204 / -3210 / -4204 / -4210)
//
// The MHS output encodes:
//   - state in the PWM base frequency (10/20/30/40/50 Hz)
//   - insulation resistance in the duty cycle (5..95 %) in Normal / Undervoltage
//
// Verify frequencies, duty ranges and the resistance formula against the
// datasheet for your exact variant (IR155-3210: Bender doc D00376).
#pragma once
#include <stdint.h>
#include <math.h>

namespace ir155 {

enum class State : uint8_t {
  NoSignal,      // no edges: IMD unpowered, wiring fault, or output shorted
  Normal,        // 10 Hz: insulation measurement valid
  Undervoltage,  // 20 Hz: HV below undervoltage threshold, measurement still reported
  SpeedStart,    // 30 Hz: fast good/bad estimate after power-on (<= ~2 s)
  DeviceError,   // 40 Hz: internal device error
  EarthFault,    // 50 Hz: E/KE chassis (Kl.31) connection fault
  Invalid        // edges present, but frequency or duty out of spec
};

struct Reading {
  State state = State::NoSignal;
  float freqHz = 0.0f;
  float dutyPct = 0.0f;
  float rKohm = -1.0f;    // insulation resistance; INFINITY = open/no leakage; -1 = not applicable
  bool sstGood = false;   // valid only in SpeedStart
  bool lineHigh = false;  // static logical level, valid only in NoSignal
};

const char* stateName(State s);

// Duty (%) -> insulation resistance (kOhm):  R = 90% * 1200k / (dc - 5%) - 1200k
// 5 % (or less) -> INFINITY (open), 95 % (or more) -> 0 (short)
float dutyToKohm(float dutyPct);

// Pure decode of one (averaged) frequency/duty pair. No hardware dependencies.
Reading decode(float freqHz, float dutyPct);

}  // namespace ir155

#ifdef ARDUINO
#include <Arduino.h>

namespace ir155 {

class Decoder {
 public:
  // invert = true when the input stage inverts (e.g. N-MOSFET with pull-up).
  void begin(uint8_t pin, bool invert);

  // Call from loop(). Non-blocking. Returns the latest stable reading.
  Reading update();

  bool readLevel() const;  // current logical level of the MHS line

 private:
  static void isr(void* arg);

  static constexpr int64_t kTimeoutUs = 250000;  // no edges for 250 ms -> NoSignal
  static constexpr int64_t kGlitchUs = 300;      // ignore edges closer than this
  static constexpr uint8_t kAvg = 4;             // periods averaged per reading
  static constexpr uint8_t kStableCount = 2;     // consecutive periods before a state change

  uint8_t pin_ = 0;
  bool invert_ = false;

  volatile int64_t tRise_ = 0, tFall_ = 0, tEdge_ = 0;
  volatile uint32_t perUs_ = 0, hiUs_ = 0;
  volatile bool fresh_ = false;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  float fBuf_[kAvg] = {0};
  float dBuf_[kAvg] = {0};
  uint8_t nBuf_ = 0, iBuf_ = 0;
  State candidate_ = State::NoSignal;
  uint8_t candCount_ = 0;
  Reading last_;
};

}  // namespace ir155
#endif
