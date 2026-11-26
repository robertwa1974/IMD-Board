#include "imd_cal.h"
#include <math.h>
#include <string.h>

namespace imd {

namespace {
constexpr float kGainTol = 0.10f;     // a channel more than 10 % off nominal = wiring mistake
constexpr float kPartTol = 0.10f;     // R0 / Rdiv more than 10 % off nominal = extra leakage or wrong Rcal
constexpr uint8_t kMaxUnsettled = 6;  // BRIDGE gives up after this many unsettled cycles

bool near(float v, float nominal, float tol) { return v > nominal * (1.0f - tol) && v < nominal * (1.0f + tol); }
}  // namespace

uint32_t crc32(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void calSeal(Calibration* cal) {
  cal->magic = Calibration::kMagic;
  cal->version = Calibration::kVersion;
  cal->crc = crc32(cal, offsetof(Calibration, crc));
}

bool calValid(const Calibration& cal) {
  if (cal.magic != Calibration::kMagic || cal.version != Calibration::kVersion) return false;
  if (cal.crc != crc32(&cal, offsetof(Calibration, crc))) return false;
  const Config nom;
  return near(cal.gVbus, 1.0f, kGainTol) && near(cal.gVe, 1.0f, kGainTol) && near(cal.gVke, 1.0f, kGainTol) &&
         near(cal.r0, nom.r0, kPartTol) && near(cal.rdiv, nom.rdiv, kPartTol);
}

Config applyCal(Config base, const Calibration& cal) {
  base.r0 = cal.r0;
  base.rdiv = cal.rdiv;
  return base;
}

// ---------------------------------------------------------------- GAIN

void GainCal::start(float vTrue, uint32_t samples) {
  vTrue_ = vTrue;
  want_ = samples;
  n_ = 0;
  sum_[0] = sum_[1] = sum_[2] = 0;
}

bool GainCal::add(float vbus, float ve, float vke) {
  if (n_ < want_) {
    sum_[0] += vbus;
    sum_[1] += ve;
    sum_[2] += vke;
    n_++;
  }
  return n_ >= want_;
}

CalResult GainCal::finish(Calibration* cal) const {
  CalResult r;
  if (n_ < want_ || n_ == 0) { r.msg = "not enough samples"; return r; }
  if (!(vTrue_ >= 30.0f && vTrue_ <= 600.0f)) { r.msg = "reference voltage must be 30..600 V"; return r; }
  float g[3];
  for (int i = 0; i < 3; i++) {
    const float mean = (float)(sum_[i] / n_);
    if (!(mean > 0.0f)) { r.msg = "a channel reads zero: check HV+, and that E and KE are tied to HV+"; return r; }
    g[i] = vTrue_ / mean;
    if (!near(g[i], 1.0f, kGainTol)) {
      r.msg = (i == 0) ? "VBUS more than 10 % off the reference: check the typed voltage and J3"
                       : "VE/VKE more than 10 % off: are E and KE both tied to HV+?";
      return r;
    }
  }
  cal->gVbus = g[0];
  cal->gVe = g[1];
  cal->gVke = g[2];
  cal->flags |= Calibration::kGain;
  calSeal(cal);
  r.ok = true;
  r.msg = "gain calibration stored";
  return r;
}

// ---------------------------------------------------------------- BRIDGE

bool solveCal(float fP, float fN, float rcal, float* r0, float* rdiv) {
  if (!(fP > 0.0f && fP < 1.0f && fN > 0.0f && fN < 1.0f && rcal > 0.0f)) return false;
  const float a = fP / (1.0f - fP);
  const float c = fN / (1.0f - fN);
  const float gp = 1.0f / rcal;
  const float gdiv = gp * (1.0f + c) / (c * (1.0f + a));
  const float g0 = a * gdiv - gp;
  if (!(gdiv > 0.0f && g0 > 0.0f)) return false;
  *r0 = 1.0f / g0;
  *rdiv = 1.0f / gdiv;
  return true;
}

void BridgeCal::start(float rcal, const Config& cfg, uint8_t cycles) {
  cfg_ = cfg;
  mon_ = Monitor(cfg);
  rcal_ = rcal;
  want_ = cycles;
  got_ = unsettled_ = 0;
  lastCycle_ = 0;
  sumP_ = sumN_ = 0;
  failed_ = false;
  failMsg_ = "";
  if (!(rcal >= 100e3f && rcal <= 5e6f)) {
    failed_ = true;
    failMsg_ = "Rcal must be 100k..5M (1M recommended)";
  }
}

Outputs BridgeCal::step(uint32_t nowMs, const Inputs& in) {
  Inputs x = in;
  x.inhibit = false;   // calibration runs regardless of INHIBIT
  x.cont1 = x.cont2 = false;
  if (failed_) return Outputs();
  if (in.vbus < cfg_.minVbus) {
    failed_ = true;
    failMsg_ = "no HV on J3";
    return Outputs();
  }
  const Outputs out = mon_.step(nowMs, x);
  const Status& st = mon_.status();
  if (st.cycles != lastCycle_) {
    lastCycle_ = st.cycles;
    if (st.cycles >= 2) {        // cycle 1 is the short speed-start cycle
      if (st.lastSettled) {
        sumP_ += st.fP;
        sumN_ += st.fN;
        got_++;
      } else if (++unsettled_ >= kMaxUnsettled) {
        failed_ = true;
        failMsg_ = "readings do not settle: remove everything but Rcal from the HV supply";
      }
    }
  }
  return done() ? Outputs() : out;   // both switches open once finished
}

CalResult BridgeCal::finish(Calibration* cal) const {
  CalResult r;
  if (failed_) { r.msg = failMsg_; return r; }
  if (got_ < want_) { r.msg = "not finished"; return r; }
  float r0, rdiv;
  if (!solveCal((float)(sumP_ / got_), (float)(sumN_ / got_), rcal_, &r0, &rdiv)) {
    r.msg = "inconsistent readings: is Rcal from HV+ to E/KE, and E/KE floating otherwise?";
    return r;
  }
  const Config nom;
  if (!near(r0, nom.r0, kPartTol)) { r.msg = "R0 more than 10 % off 480k: check the Rcal value and its wiring"; return r; }
  if (!near(rdiv, nom.rdiv, kPartTol)) { r.msg = "Rdiv more than 10 % off: E/KE leaking to HV- (bench earth?)"; return r; }
  cal->r0 = r0;
  cal->rdiv = rdiv;
  cal->flags |= Calibration::kBridge;
  calSeal(cal);
  r.ok = true;
  r.msg = "bridge calibration stored";
  return r;
}

}  // namespace imd
