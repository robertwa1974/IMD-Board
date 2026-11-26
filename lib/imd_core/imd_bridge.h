// Switched-resistor bridge maths (no hardware dependencies, host-testable).
//
// Node model, all voltages relative to HV-:
//   Vbus = HV+ to HV-,  Vn = chassis to HV-,  Vp = Vbus - Vn.
//   Gp = 1/Rp (HV+ to chassis leakage), Gn = 1/Rn (chassis to HV- leakage),
//   Gdiv = 1/Rdiv (this board's own chassis-to-HV- sense dividers, E || KE),
//   G0 = 1/R0 (the switched bridge resistor).
//
// Settled chassis fraction f = Vn / Vbus, and ratio r = Vn / Vp = f / (1 - f).
//   Phase P (R0 from HV+ to chassis):  a = (Gp + G0) / x            with x = Gn + Gdiv
//   Phase N (R0 from chassis to HV-):  c = Gp / (x + G0)
// Solving:  x = G0 (1 + c) / (a - c),   Gp = c (x + G0),   Gn = x - Gdiv.
// (Equivalent to HANDOFF.md section 2 with A = 1/a, C = 1/c.)
#pragma once
#include <math.h>

namespace imd {

struct Insulation {
  float rp = INFINITY;  // HV+ to chassis, ohms (INFINITY = no measurable leakage)
  float rn = INFINITY;  // chassis to HV-, ohms
  bool ok = false;      // false: the two phases cannot be told apart (switch fault?)
};

// Minimum a - c for a usable solution. With R0 = 480k, Rdiv = 2.4M the real difference is
// >= 0.04 down to Rp = Rn = 10 kOhm; a stuck R0 switch gives ~0.
constexpr float kMinPhaseDelta = 0.01f;

// fP, fN: settled Vn/Vbus in phase P and N (0..1).
Insulation solveBridge(float fP, float fN, float r0, float rdiv);

// Forward model: settled Vn/Vbus for a given phase. r0p / r0n select which switch is on.
// Used by the tests and by firmware plausibility checks.
float settledFraction(float rp, float rn, float r0, float rdiv, bool r0p, bool r0n);

// Three equally spaced samples of a first-order settling curve -> final value.
// Returns false (and leaves *out untouched) if the samples are not a clean exponential
// approach (non-monotonic, flat, or an implausible result).
bool extrapolate(float v1, float v2, float v3, float* out);

}  // namespace imd
