#include "imd_bridge.h"

namespace imd {

static float ratioOf(float f) {
  // Vn/Vp. Clamp so a chassis at (or past) HV+ gives a huge but finite ratio.
  if (f < 0.0f) f = 0.0f;
  if (f > 0.999999f) f = 0.999999f;
  return f / (1.0f - f);
}

Insulation solveBridge(float fP, float fN, float r0, float rdiv) {
  Insulation out;
  const float g0 = 1.0f / r0;
  const float gdiv = 1.0f / rdiv;
  const float a = ratioOf(fP);
  const float c = ratioOf(fN);
  if (!(a - c > kMinPhaseDelta)) return out;  // also rejects NaN

  const float x = g0 * (1.0f + c) / (a - c);
  float gp = c * (x + g0);
  float gn = x - gdiv;
  // Noise can push a tiny conductance slightly negative: that is "no leakage".
  if (gp < 0.0f) gp = 0.0f;
  if (gn < 0.0f) gn = 0.0f;
  out.rp = gp > 0.0f ? 1.0f / gp : INFINITY;
  out.rn = gn > 0.0f ? 1.0f / gn : INFINITY;
  out.ok = true;
  return out;
}

float settledFraction(float rp, float rn, float r0, float rdiv, bool r0p, bool r0n) {
  float gTop = (isinf(rp) ? 0.0f : 1.0f / rp) + (r0p ? 1.0f / r0 : 0.0f);
  float gBot = (isinf(rn) ? 0.0f : 1.0f / rn) + 1.0f / rdiv + (r0n ? 1.0f / r0 : 0.0f);
  return gTop / (gTop + gBot);
}

bool extrapolate(float v1, float v2, float v3, float* out) {
  const float d1 = v2 - v1;
  const float d2 = v3 - v2;
  // Must keep moving the same way and slow down (decaying exponential).
  if (d1 == 0.0f || d1 * d2 <= 0.0f || fabsf(d2) >= fabsf(d1)) return false;
  const float den = v1 + v3 - 2.0f * v2;  // = d2 - d1, non-zero here
  const float vInf = (v1 * v3 - v2 * v2) / den;
  // The final value lies beyond v3 in the direction of travel, and not absurdly far:
  // with ratio q = d2/d1 the remaining step is d2*q/(1-q). Cap q at 0.95 (~20 windows/tau).
  const float q = d2 / d1;
  if (q > 0.95f) return false;
  if ((vInf - v3) * d2 < 0.0f) return false;
  *out = vInf;
  return true;
}

}  // namespace imd
