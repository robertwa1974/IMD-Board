// Bender IR155-compatible MHS output encoding (inverse of lib/ir155_decoder).
// VERIFY frequencies, duty ranges and formula against Bender D00376 before relying on them.
#pragma once
#include <stdint.h>

namespace imd {

enum class MhsMode : uint8_t {
  Off,             // output held low (power-up, before any estimate)
  Normal,          // 10 Hz, duty 5..95 % encodes resistance
  Undervoltage,    // 20 Hz, same duty encoding
  SpeedStartGood,  // 30 Hz, 5..10 %
  SpeedStartBad,   // 30 Hz, 90..95 %
  DeviceError,     // 40 Hz, 50 %
  EarthFault,      // 50 Hz, 50 %
};

struct PwmCmd {
  uint8_t freqHz = 0;    // 0 = output constantly low
  float dutyPct = 0.0f;
  bool operator==(const PwmCmd& o) const { return freqHz == o.freqHz && dutyPct == o.dutyPct; }
  bool operator!=(const PwmCmd& o) const { return !(*this == o); }
};

// R (kOhm) -> duty: inverse of R = 90% * 1200k / (duty - 5%) - 1200k.
// INFINITY -> 5 %, 0 -> 95 %.
float kohmToDuty(float rKohm);

// rKohm only used for Normal / Undervoltage.
PwmCmd encodeMhs(MhsMode mode, float rKohm);

const char* mhsModeName(MhsMode m);

}  // namespace imd
