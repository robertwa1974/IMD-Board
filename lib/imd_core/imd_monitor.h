// IMD measurement state machine (no hardware dependencies, host-testable).
//
// Call Monitor::step() once per fresh ADC sample set (VBUS, VE, VKE in volts at the pack side
// of the dividers, VISO in volts). It returns which R0 switch to close and what the MHS / OKHS
// outputs should show. Cycle: phase P (R0 HV+ -> chassis) until settled, then phase N
// (R0 chassis -> HV-) until settled, then solve, decide, repeat. The very first cycle after
// power-up uses short phases and extrapolation to give an IR155-style "speed start" estimate.
#pragma once
#include <stdint.h>
#include "imd_bridge.h"
#include "imd_mhs.h"

namespace imd {

struct Config {
  float r0 = 480e3f;              // bridge resistor (4 x 120k)
  float rdiv = 2.409e6f;          // E || KE sense dividers, each 4 x 1.2M + 18k
  float responseOhmPerV = 100.0f; // OKHS drops below this x Vbus (FMVSS 305 / ECE R100 DC limit)
  float hysteresis = 0.25f;       // OKHS returns at threshold x (1 + hysteresis)
  uint8_t tripCycles = 2;         // consecutive low results before OKHS drops (spike rejection)
  float minVbus = 20.0f;          // below: no HV or HV tap lost -> device error
  float uvThreshold = 0.0f;       // undervoltage indication (0 = off, like the IR155-3210 default)
  float visoMin = 4.5f, visoMax = 5.6f;  // isolated supply window (UCC12050 5.0 V mode)
  uint32_t windowMs = 100;        // averaging window inside a phase
  uint32_t minPhaseMs = 400;
  uint32_t maxPhaseMs = 4000;     // raise this for large Y-capacitance (measure it first!)
  uint32_t sstPhaseMs = 600;      // speed-start cycle phase length
  float settleTol = 0.0005f;      // in Vn/Vbus: 0.2 V at 400 V
  float earthMismatchV = 10.0f;   // E vs KE difference that counts as a broken chassis wire...
  float earthMismatchFrac = 0.05f;//   ...or this fraction of Vbus, whichever is larger
  uint8_t earthFaultCycles = 2;
  uint8_t badCyclesForError = 3;  // unsolvable cycles in a row -> device error
  // Pack-voltage transients (acceleration, regen): the chassis node lags a VBUS change through the
  // Y-capacitance, and the ADC reads VBUS and VE ~11 ms apart. A phase is only used if VBUS stayed
  // within vbusStableFrac for the whole phase; otherwise the phase starts again.
  float vbusStableFrac = 0.005f;
  // After maxRestarts restarts in a row, finish the phase anyway with a looser limit and twice the
  // averaging window (reported as a "relaxed" cycle) - e.g. steady driving with ripple on the pack.
  uint8_t maxRestarts = 5;
  float vbusStableRelaxed = 0.02f;
  // No valid measurement for this long while HV is live and not inhibited -> "stale" warning.
  // Outputs keep showing the last valid result (a warning, not a fault).
  uint32_t staleMs = 60000;
};

struct Inputs {
  float vbus = 0, ve = 0, vke = 0, viso = 0;
  float vbusDelta = 0;            // |VBUS read before VE - VBUS read after VKE| in this sample set
  bool cont1 = false, cont2 = false, inhibit = false;
};

struct Outputs {
  bool r0p = false, r0n = false;
  PwmCmd mhs;
  bool okhs = false;  // also drives IMD_OK
};

enum class Phase : uint8_t { P, N, Inhibited };

struct Status {
  MhsMode mode = MhsMode::Off;
  Phase phase = Phase::P;
  bool haveResult = false;
  float rp = INFINITY, rn = INFINITY, riso = INFINITY;  // ohms, last valid
  float fP = 0, fN = 0;      // last settled Vn/Vbus per phase
  float vbus = 0;
  bool systemScope = false;  // true when a contactor was closed for the whole last cycle
  bool insOk = false;        // insulation above threshold (with hysteresis)
  bool sstGood = false;      // speed-start verdict (first cycle only)
  uint32_t cycles = 0;
  uint8_t earthCount = 0, badCount = 0, lowCount = 0;
  bool lastSettled = false;  // last cycle settled (vs. timed out)
  bool lastRelaxed = false;  // last cycle needed the relaxed VBUS-stability limit
  bool stale = false;        // no valid measurement for cfg.staleMs while HV live (degraded)
  uint32_t ageMs = 0;        // time since the last valid measurement
  uint8_t restarts = 0;      // consecutive VBUS-transient restarts of the current phase
  uint32_t transientRestarts = 0;  // total, for diagnostics
};

// Tracks one phase: windowed means of f = Vn/Vbus, settles or times out.
class PhaseTracker {
 public:
  void start(uint32_t t, uint32_t minMs, uint32_t maxMs, uint32_t windowMs, float tol);
  bool add(uint32_t t, float f);  // true when the phase is finished
  float result() const { return result_; }
  bool settled() const { return settled_; }

 private:
  void finish(bool settled);
  uint32_t t0_ = 0, tw_ = 0, minMs_ = 0, maxMs_ = 0, winMs_ = 100;
  float tol_ = 0.0005f;
  float sum_ = 0;
  uint32_t n_ = 0;
  float hist_[3] = {0, 0, 0};
  uint8_t nh_ = 0;
  float result_ = 0;
  bool settled_ = false;
};

class Monitor {
 public:
  explicit Monitor(const Config& cfg = Config()) : cfg_(cfg) {}
  Outputs step(uint32_t nowMs, const Inputs& in);
  const Status& status() const { return st_; }
  const Config& config() const { return cfg_; }

 private:
  void restart(uint32_t now);
  void startPhase(Phase p, uint32_t now);
  void finishCycle(const Inputs& in, uint32_t now);
  void updateOutputs();

  Config cfg_;
  Status st_;
  Outputs out_;
  PhaseTracker trk_;
  bool started_ = false;
  bool scopeAtStart_ = false;
  bool earthMismatch_ = false;
  bool sstDone_ = false;
  float vbusMin_ = 0, vbusMax_ = 0;   // VBUS range seen in the current phase
  bool cycleRelaxed_ = false;
  uint32_t lastValidMs_ = 0;
};

}  // namespace imd
