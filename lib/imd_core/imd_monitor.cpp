#include "imd_monitor.h"

namespace imd {

// ---------------------------------------------------------------- PhaseTracker

void PhaseTracker::start(uint32_t t, uint32_t minMs, uint32_t maxMs, uint32_t windowMs, float tol) {
  t0_ = tw_ = t;
  minMs_ = minMs;
  maxMs_ = maxMs;
  winMs_ = windowMs;
  tol_ = tol;
  sum_ = 0;
  n_ = 0;
  nh_ = 0;
  result_ = 0;
  settled_ = false;
}

void PhaseTracker::finish(bool settled) {
  settled_ = settled;
  if (nh_ >= 3) {
    float v;
    // Settled or not, a clean exponential tail is the best estimate of the final value.
    if (extrapolate(hist_[0], hist_[1], hist_[2], &v)) {
      result_ = v;
      return;
    }
    // Noisy: average the last windows instead of trusting one.
    result_ = settled ? hist_[2] : (hist_[0] + hist_[1] + hist_[2]) / 3.0f;
  } else if (nh_ > 0) {
    result_ = hist_[nh_ - 1];
  } else {
    result_ = n_ ? sum_ / (float)n_ : 0.0f;
  }
}

bool PhaseTracker::add(uint32_t t, float f) {
  sum_ += f;
  n_++;
  if (t - tw_ >= winMs_ && n_ > 0) {
    const float m = sum_ / (float)n_;
    if (nh_ < 3) {
      hist_[nh_++] = m;
    } else {
      hist_[0] = hist_[1];
      hist_[1] = hist_[2];
      hist_[2] = m;
    }
    sum_ = 0;
    n_ = 0;
    tw_ = t;

    if (t - t0_ >= minMs_ && nh_ >= 3) {
      float v;
      const bool flat = fabsf(hist_[2] - hist_[1]) < tol_ * 0.25f;
      const bool nearEnd = extrapolate(hist_[0], hist_[1], hist_[2], &v) && fabsf(v - hist_[2]) < tol_;
      if (flat || nearEnd) {
        finish(true);
        return true;
      }
    }
  }
  if (t - t0_ >= maxMs_) {
    finish(false);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- Monitor

void Monitor::startPhase(Phase p, uint32_t now) {
  st_.phase = p;
  const bool sst = !sstDone_;
  const bool relaxed = st_.restarts >= cfg_.maxRestarts;
  const uint32_t maxMs = sst ? cfg_.sstPhaseMs : cfg_.maxPhaseMs;
  const uint32_t minMs = sst ? cfg_.sstPhaseMs / 2 : cfg_.minPhaseMs;
  // Relaxed: heavier averaging (twice the window) to make up for the looser VBUS limit.
  trk_.start(now, minMs, maxMs, relaxed ? cfg_.windowMs * 2 : cfg_.windowMs, cfg_.settleTol);
  vbusMin_ = 1e9f;
  vbusMax_ = -1e9f;
  out_.r0p = (p == Phase::P);
  out_.r0n = (p == Phase::N);
}

void Monitor::restart(uint32_t now) {
  earthMismatch_ = false;
  cycleRelaxed_ = false;
  st_.restarts = 0;
  startPhase(Phase::P, now);
}

Outputs Monitor::step(uint32_t now, const Inputs& in) {
  const bool scope = in.cont1 || in.cont2;
  if (!started_) {
    started_ = true;
    scopeAtStart_ = scope;
    lastValidMs_ = now;
    restart(now);
  }
  st_.vbus = in.vbus;

  // DC fast charging etc.: both switches open, hold the last reported state.
  if (in.inhibit) {
    st_.phase = Phase::Inhibited;
    out_.r0p = out_.r0n = false;
    lastValidMs_ = now;   // not measuring on purpose: the held reading does not age
    st_.stale = false;
    return out_;
  }
  if (st_.phase == Phase::Inhibited) {
    scopeAtStart_ = scope;
    restart(now);
  }

  // Contactors changed: the measured system changed (motor-side Y-caps and leakage joined or
  // left), so the current cycle is meaningless. Start again.
  if (scope != scopeAtStart_) {
    scopeAtStart_ = scope;
    restart(now);
  }

  // Hard device faults: no usable HV reference, or isolated supply out of range.
  const bool visoBad = in.viso < cfg_.visoMin || in.viso > cfg_.visoMax;
  const bool vbusLow = in.vbus < cfg_.minVbus;
  if (visoBad || vbusLow) {
    st_.mode = MhsMode::DeviceError;
    updateOutputs();
    restart(now);  // keep switching so the self-test keeps running, but use no data
    lastValidMs_ = now;   // no HV to measure: staleness is not the issue (device error already is)
    st_.stale = false;
    return out_;
  }

  // Held-reading age: HV is live and we are trying to measure.
  st_.ageMs = now - lastValidMs_;
  st_.stale = st_.ageMs > cfg_.staleMs;

  // Pack-voltage transient: restart this phase rather than average across it.
  const bool relaxed = st_.restarts >= cfg_.maxRestarts;
  const float vlim = (relaxed ? cfg_.vbusStableRelaxed : cfg_.vbusStableFrac) * in.vbus;
  vbusMin_ = fminf(vbusMin_, in.vbus);
  vbusMax_ = fmaxf(vbusMax_, in.vbus);
  if (vbusMax_ - vbusMin_ > vlim || in.vbusDelta > vlim) {
    if (st_.restarts < 255) st_.restarts++;
    st_.transientRestarts++;
    startPhase(st_.phase, now);
    return out_;
  }

  float f = in.ve / in.vbus;
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;

  if (trk_.add(now, f)) {
    cycleRelaxed_ = cycleRelaxed_ || relaxed;
    st_.restarts = 0;
    if (st_.phase == Phase::P) {
      st_.fP = trk_.result();
      // Broken E or KE: during phase P, R0 pulls E towards HV+, so a broken E reads high and a
      // broken KE reads ~0 while the other follows the real chassis.
      const float lim = fmaxf(cfg_.earthMismatchV, cfg_.earthMismatchFrac * in.vbus);
      earthMismatch_ = fabsf(in.ve - in.vke) > lim;
      st_.lastSettled = trk_.settled();
      startPhase(Phase::N, now);
    } else {
      st_.fN = trk_.result();
      st_.lastSettled = st_.lastSettled && trk_.settled();
      finishCycle(in, now);
      startPhase(Phase::P, now);
      earthMismatch_ = false;
    }
  }
  return out_;
}

void Monitor::finishCycle(const Inputs& in, uint32_t now) {
  st_.cycles++;
  st_.systemScope = scopeAtStart_;
  st_.lastRelaxed = cycleRelaxed_;
  cycleRelaxed_ = false;
  const bool sst = !sstDone_;
  sstDone_ = true;

  st_.earthCount = earthMismatch_ ? (uint8_t)(st_.earthCount + 1) : 0;

  const Insulation ins = solveBridge(st_.fP, st_.fN, cfg_.r0, cfg_.rdiv);
  if (ins.ok) {
    st_.badCount = 0;
    st_.rp = ins.rp;
    st_.rn = ins.rn;
    st_.riso = fminf(ins.rp, ins.rn);
    st_.haveResult = true;
    lastValidMs_ = now;
    st_.ageMs = 0;
    st_.stale = false;

    const float thr = cfg_.responseOhmPerV * in.vbus;
    if (sst) st_.sstGood = st_.riso >= thr;
    if (st_.riso < thr) {
      if (st_.lowCount < 255) st_.lowCount++;
      if (st_.lowCount >= cfg_.tripCycles || sst) st_.insOk = false;
    } else {
      st_.lowCount = 0;
      if (st_.riso >= thr * (1.0f + cfg_.hysteresis)) st_.insOk = true;
    }
  } else if (st_.badCount < 255) {
    st_.badCount++;
  }

  if (st_.earthCount >= cfg_.earthFaultCycles) {
    st_.mode = MhsMode::EarthFault;
  } else if (st_.badCount >= cfg_.badCyclesForError) {
    st_.mode = MhsMode::DeviceError;
  } else if (!ins.ok) {
    // Hold the last valid state (noise spike / one bad cycle).
  } else if (sst) {
    st_.mode = st_.sstGood ? MhsMode::SpeedStartGood : MhsMode::SpeedStartBad;
  } else if (cfg_.uvThreshold > 0.0f && in.vbus < cfg_.uvThreshold) {
    st_.mode = MhsMode::Undervoltage;
  } else {
    st_.mode = MhsMode::Normal;
  }
  updateOutputs();
}

void Monitor::updateOutputs() {
  out_.mhs = encodeMhs(st_.mode, st_.riso / 1000.0f);
  // OKHS / IMD_OK: only a full, valid, in-range measurement may report OK.
  out_.okhs = st_.mode == MhsMode::Normal && st_.insOk;
}

}  // namespace imd
