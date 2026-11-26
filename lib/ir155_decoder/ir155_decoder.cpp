#include "ir155_decoder.h"

namespace ir155 {

const char* stateName(State s) {
  switch (s) {
    case State::NoSignal:     return "NO_SIGNAL";
    case State::Normal:       return "NORMAL";
    case State::Undervoltage: return "UNDERVOLTAGE";
    case State::SpeedStart:   return "SPEED_START";
    case State::DeviceError:  return "DEVICE_ERROR";
    case State::EarthFault:   return "EARTH_FAULT";
    case State::Invalid:      return "INVALID";
  }
  return "?";
}

float dutyToKohm(float dutyPct) {
  if (dutyPct <= 5.0f) return INFINITY;
  if (dutyPct >= 95.0f) return 0.0f;
  float r = (90.0f * 1200.0f) / (dutyPct - 5.0f) - 1200.0f;
  return r < 0.0f ? 0.0f : r;
}

// Nearest 10 Hz step, accepted if within +/-3 Hz. Returns 0 if not a valid code.
static int freqClass(float f) {
  int nearest = (int)lroundf(f / 10.0f) * 10;
  if (nearest < 10 || nearest > 50) return 0;
  return fabsf(f - (float)nearest) <= 3.0f ? nearest : 0;
}

Reading decode(float freqHz, float dutyPct) {
  Reading r;
  r.freqHz = freqHz;
  r.dutyPct = dutyPct;
  r.state = State::Invalid;

  switch (freqClass(freqHz)) {
    case 10:
    case 20:
      // Small tolerance around the 5..95 % window for timing jitter
      if (dutyPct >= 3.0f && dutyPct <= 97.0f) {
        r.state = (freqClass(freqHz) == 10) ? State::Normal : State::Undervoltage;
        r.rKohm = dutyToKohm(dutyPct);
      }
      break;
    case 30:  // Speed start: 5..10 % good, 90..95 % bad
      if (dutyPct <= 15.0f) { r.state = State::SpeedStart; r.sstGood = true; }
      else if (dutyPct >= 85.0f) { r.state = State::SpeedStart; r.sstGood = false; }
      break;
    case 40:  // Device error: ~50 % duty
      if (dutyPct >= 40.0f && dutyPct <= 60.0f) r.state = State::DeviceError;
      break;
    case 50:  // Chassis connection (Kl.31 / E-KE) fault: ~50 % duty
      if (dutyPct >= 40.0f && dutyPct <= 60.0f) r.state = State::EarthFault;
      break;
    default:
      break;
  }
  return r;
}

}  // namespace ir155

#ifdef ARDUINO
#include "soc/gpio_reg.h"
#include "esp_timer.h"

namespace ir155 {

// IRAM-safe raw pin read (digitalRead is not guaranteed to be in IRAM)
static inline bool IRAM_ATTR rawLevel(uint8_t pin) {
  if (pin < 32) return (REG_READ(GPIO_IN_REG) >> pin) & 1U;
  return (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1U;
}

void Decoder::begin(uint8_t pin, bool invert) {
  pin_ = pin;
  invert_ = invert;
  pinMode(pin_, INPUT);  // conditioning stage provides the pull-up / pull-down
  attachInterruptArg(digitalPinToInterrupt(pin_), Decoder::isr, this, CHANGE);
}

bool Decoder::readLevel() const { return rawLevel(pin_) ^ invert_; }

void IRAM_ATTR Decoder::isr(void* arg) {
  Decoder* d = static_cast<Decoder*>(arg);
  int64_t now = esp_timer_get_time();
  bool level = rawLevel(d->pin_) ^ d->invert_;

  portENTER_CRITICAL_ISR(&d->mux_);
  if (d->tEdge_ != 0 && (now - d->tEdge_) < kGlitchUs) {
    portEXIT_CRITICAL_ISR(&d->mux_);
    return;
  }
  d->tEdge_ = now;
  if (level) {  // rising edge: a full period just completed
    if (d->tRise_ != 0 && d->tFall_ > d->tRise_) {
      d->perUs_ = (uint32_t)(now - d->tRise_);
      d->hiUs_ = (uint32_t)(d->tFall_ - d->tRise_);
      d->fresh_ = true;
    }
    d->tRise_ = now;
  } else {
    d->tFall_ = now;
  }
  portEXIT_CRITICAL_ISR(&d->mux_);
}

Reading Decoder::update() {
  uint32_t per, hi;
  bool fresh;
  int64_t tEdge;
  portENTER_CRITICAL(&mux_);
  per = perUs_;
  hi = hiUs_;
  fresh = fresh_;
  fresh_ = false;
  tEdge = tEdge_;
  portEXIT_CRITICAL(&mux_);

  int64_t now = esp_timer_get_time();
  if (tEdge == 0 || (now - tEdge) > kTimeoutUs) {
    Reading r;
    r.state = State::NoSignal;
    r.lineHigh = readLevel();
    last_ = r;
    candidate_ = State::NoSignal;
    candCount_ = 0;
    nBuf_ = 0;
    return last_;
  }
  if (!fresh || per == 0) return last_;

  float f = 1e6f / (float)per;
  float duty = 100.0f * (float)hi / (float)per;
  State s = decode(f, duty).state;

  if (s != candidate_) {  // new state: restart averaging and stability count
    candidate_ = s;
    candCount_ = 1;
    nBuf_ = 0;
    iBuf_ = 0;
  } else if (candCount_ < 255) {
    candCount_++;
  }

  fBuf_[iBuf_] = f;
  dBuf_[iBuf_] = duty;
  iBuf_ = (iBuf_ + 1) % kAvg;
  if (nBuf_ < kAvg) nBuf_++;

  if (candCount_ >= kStableCount) {
    float fs = 0, ds = 0;
    for (uint8_t i = 0; i < nBuf_; i++) { fs += fBuf_[i]; ds += dBuf_[i]; }
    last_ = decode(fs / nBuf_, ds / nBuf_);
  }
  return last_;
}

}  // namespace ir155
#endif
