// One-time bench calibration (no hardware dependencies, host-testable).
//
// Why: the HV strings and the 18k divider bottoms are 1 % parts, so the three ADC channels match
// only to ~1 %, and R0 / Rdiv are only known to ~1 %. Near the 100 ohm/V threshold the bridge solve
// amplifies those errors. Two steps fix it (procedure in DESIGN_NOTES.md "Calibration"):
//
//   GAIN   E and KE tied to HV+, R0 switches open: all three channels see the same, known voltage.
//          -> one gain factor per channel.
//   BRIDGE A known resistor Rcal from HV+ to the (floating) E/KE node, nothing else connected:
//          Rp = Rcal and Rn is open, so one P/N cycle gives two equations for the board's own
//          R0 and Rdiv:  Gdiv = Gp (1 + c) / (c (1 + a)),  G0 = a Gdiv - Gp
//          (a, c = Vn/Vp in phase P and N, Gp = 1/Rcal; same model as imd_bridge.h).
// Run GAIN before BRIDGE: the bridge step measures with the current gains.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "imd_monitor.h"

namespace imd {

struct Calibration {
  static constexpr uint32_t kMagic = 0x43444D49;  // "IMDC"
  static constexpr uint16_t kVersion = 1;
  enum : uint16_t { kGain = 1, kBridge = 2 };

  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t flags = 0;                       // which steps have been done
  float gVbus = 1.0f, gVe = 1.0f, gVke = 1.0f;  // multiply the divider-scaled readings
  float r0 = 480e3f;                        // measured R0 (ohms)
  float rdiv = 2.409e6f;                    // measured E || KE divider (ohms)
  uint32_t crc = 0;                         // CRC-32 of everything above
};

uint32_t crc32(const void* data, size_t len);
void calSeal(Calibration* cal);             // fills in the CRC
bool calValid(const Calibration& cal);      // magic, version, CRC and sane values
Config applyCal(Config base, const Calibration& cal);

struct CalResult {
  bool ok = false;
  const char* msg = "";
};

// GAIN step: feed divider-scaled volts (before gain correction) with both R0 switches open.
class GainCal {
 public:
  void start(float vTrue, uint32_t samples = 100);
  bool add(float vbus, float ve, float vke);  // true once enough samples are in
  CalResult finish(Calibration* cal) const;

 private:
  float vTrue_ = 0;
  uint32_t want_ = 0, n_ = 0;
  double sum_[3] = {0, 0, 0};
};

// Closed form for the BRIDGE step (Rp = rcal, Rn open). False if the numbers are not physical.
bool solveCal(float fP, float fN, float rcal, float* r0, float* rdiv);

// BRIDGE step: runs the normal P/N measurement and averages settled cycles.
class BridgeCal {
 public:
  void start(float rcal, const Config& cfg, uint8_t cycles = 4);
  Outputs step(uint32_t nowMs, const Inputs& in);  // gain-corrected inputs, like Monitor::step
  bool done() const { return failed_ || got_ >= want_; }
  CalResult finish(Calibration* cal) const;
  const Monitor& monitor() const { return mon_; }

 private:
  Monitor mon_;
  Config cfg_;
  float rcal_ = 0;
  uint8_t want_ = 0, got_ = 0, unsettled_ = 0;
  uint32_t lastCycle_ = 0;
  double sumP_ = 0, sumN_ = 0;
  bool failed_ = false;
  const char* failMsg_ = "";
};

}  // namespace imd
