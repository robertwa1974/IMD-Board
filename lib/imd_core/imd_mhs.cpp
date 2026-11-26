#include "imd_mhs.h"
#include <math.h>

namespace imd {

float kohmToDuty(float rKohm) {
  if (isnan(rKohm)) return 95.0f;  // fail towards "bad"
  if (isinf(rKohm)) return 5.0f;
  if (rKohm < 0.0f) rKohm = 0.0f;
  float d = 5.0f + (90.0f * 1200.0f) / (rKohm + 1200.0f);
  if (d > 95.0f) d = 95.0f;
  if (d < 5.0f) d = 5.0f;
  return d;
}

PwmCmd encodeMhs(MhsMode mode, float rKohm) {
  PwmCmd c;
  switch (mode) {
    case MhsMode::Off:            break;
    case MhsMode::Normal:         c.freqHz = 10; c.dutyPct = kohmToDuty(rKohm); break;
    case MhsMode::Undervoltage:   c.freqHz = 20; c.dutyPct = kohmToDuty(rKohm); break;
    case MhsMode::SpeedStartGood: c.freqHz = 30; c.dutyPct = 7.5f; break;
    case MhsMode::SpeedStartBad:  c.freqHz = 30; c.dutyPct = 92.5f; break;
    case MhsMode::DeviceError:    c.freqHz = 40; c.dutyPct = 50.0f; break;
    case MhsMode::EarthFault:     c.freqHz = 50; c.dutyPct = 50.0f; break;
  }
  return c;
}

const char* mhsModeName(MhsMode m) {
  switch (m) {
    case MhsMode::Off:            return "OFF";
    case MhsMode::Normal:         return "NORMAL";
    case MhsMode::Undervoltage:   return "UNDERVOLTAGE";
    case MhsMode::SpeedStartGood: return "SST_GOOD";
    case MhsMode::SpeedStartBad:  return "SST_BAD";
    case MhsMode::DeviceError:    return "DEVICE_ERROR";
    case MhsMode::EarthFault:     return "EARTH_FAULT";
  }
  return "?";
}

}  // namespace imd
