// Host tests for lib/imd_core: bridge maths, MHS encoding (round trip through the IR155
// decoder), and the Monitor state machine against a simulated vehicle.
// Run: pio test -e native
#include <unity.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "imd_bridge.h"
#include "imd_cal.h"
#include "imd_mhs.h"
#include "imd_monitor.h"
#include "ir155_decoder.h"

using namespace imd;

void setUp() {}
void tearDown() {}

// ------------------------------------------------------------------ simulated vehicle
// Chassis node relative to HV-, with Y-capacitance, leakage Rp/Rn, the board's two chassis
// dividers (E and KE wires) and the switched R0. Exact exponential update per time step.
struct Plant {
  float vbus = 400.0f;
  float rp = 20e6f, rn = 20e6f;
  float cy = 200e-9f;           // total Y-capacitance (both rails, they are in parallel here)
  float r0 = 480e3f;
  float rdivE = 4.818e6f, rdivKE = 4.818e6f;
  bool eBroken = false, keBroken = false, switchDead = false;
  bool eTiedToHvp = false;            // calibration wiring: E and KE jumpered to HV+
  float err[3] = {1.0f, 1.0f, 1.0f};  // per-channel gain error of the real hardware (VBUS, VE, VKE)
  float noise = 0.0f;           // +/- volts of pseudo-random noise on every reading
  float vch = 0.0f;             // chassis voltage
  uint32_t seed = 12345;

  static float g(float r) { return isinf(r) ? 0.0f : 1.0f / r; }

  // Pack voltage step (acceleration / regen): with symmetric Y-capacitors the chassis node takes
  // half the step at once, then settles back to the resistive ratio with the RC time constant.
  void setVbus(float v) {
    vch += (v - vbus) * 0.5f;
    vbus = v;
  }

  void advance(float dt, bool r0p, bool r0n) {
    const bool sw = !switchDead && !eBroken;  // R0 reaches the chassis only through E
    const float gTop = g(rp) + (r0p && sw ? 1.0f / r0 : 0.0f);
    const float gBot = g(rn) + (eBroken ? 0.0f : 1.0f / rdivE) + (keBroken ? 0.0f : 1.0f / rdivKE) +
                       (r0n && sw ? 1.0f / r0 : 0.0f);
    const float vInf = vbus * gTop / (gTop + gBot);
    const float tau = cy / (gTop + gBot);
    vch = vInf + (vch - vInf) * expf(-dt / tau);
  }

  float rnd() {
    seed = seed * 1103515245u + 12345u;
    return ((float)((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 2.0f * noise;
  }

  Inputs read(bool r0p, bool r0n) {
    Inputs in;
    in.vbus = vbus + rnd();
    if (eBroken) {  // E node: only its own divider and the R0 switches
      const float gt = (r0p && !switchDead) ? 1.0f / r0 : 0.0f;
      const float gb = 1.0f / rdivE + ((r0n && !switchDead) ? 1.0f / r0 : 0.0f);
      in.ve = vbus * gt / (gt + gb) + rnd();
    } else {
      in.ve = vch + rnd();
    }
    in.vke = keBroken ? rnd() : vch + rnd();
    if (eTiedToHvp) in.ve = in.vke = vbus + rnd();
    in.vbus *= err[0];
    in.ve *= err[1];
    in.vke *= err[2];
    in.viso = 5.0f;
    return in;
  }
};

// Runs the monitor against the plant. One ADC sample set every 35 ms (3 x 90 SPS).
struct Sim {
  Plant plant;
  Monitor mon;
  Outputs out;
  uint32_t t = 0;
  bool cont1 = false, inhibit = false;
  float corr[3] = {1.0f, 1.0f, 1.0f};  // gain correction applied by the firmware (from calibration)
  float rippleV = 0, ripplePeriodS = 0.4f, vbusBase = 0;  // square-wave pack-voltage swing
  explicit Sim(const Config& c = Config()) : mon(c) {}
  void setRipple(float volts) {
    if (rippleV == 0) vbusBase = plant.vbus;
    rippleV = volts;
    if (volts == 0) plant.setVbus(vbusBase);
  }
  Inputs sample() {
    if (rippleV > 0) {
      const bool hi = fmodf(t / 1000.0f, ripplePeriodS) < ripplePeriodS / 2;
      plant.setVbus(vbusBase + (hi ? rippleV : -rippleV));
    }
    plant.advance(0.035f, out.r0p, out.r0n);
    Inputs in = plant.read(out.r0p, out.r0n);
    in.vbus *= corr[0];
    in.ve *= corr[1];
    in.vke *= corr[2];
    t += 35;
    return in;
  }
  void run(float seconds) {
    const uint32_t steps = (uint32_t)(seconds * 1000.0f / 35.0f);
    for (uint32_t i = 0; i < steps; i++) {
      Inputs in = sample();
      in.cont1 = cont1;
      in.inhibit = inhibit;
      out = mon.step(t, in);
    }
  }
};

static void assertRel(float expected, float actual, float rel) {
  if (isinf(expected)) {
    TEST_ASSERT_TRUE_MESSAGE(actual > 1e9f, "expected open (infinite) resistance");
    return;
  }
  TEST_ASSERT_FLOAT_WITHIN(expected * rel, expected, actual);
}

// ------------------------------------------------------------------ bridge maths

static void test_solve_matches_forward_model() {
  const float r0 = 480e3f, rdiv = 2.409e6f;
  const float vals[] = {INFINITY, 20e6f, 1e6f, 200e3f, 40e3f, 10e3f};
  for (float rp : vals) {
    for (float rn : vals) {
      const float fP = settledFraction(rp, rn, r0, rdiv, true, false);
      const float fN = settledFraction(rp, rn, r0, rdiv, false, true);
      const Insulation ins = solveBridge(fP, fN, r0, rdiv);
      TEST_ASSERT_TRUE(ins.ok);
      assertRel(rp, ins.rp, 0.01f);
      assertRel(rn, ins.rn, 0.01f);
    }
  }
}

static void test_solve_rejects_identical_phases() {
  TEST_ASSERT_FALSE(solveBridge(0.3f, 0.3f, 480e3f, 2.409e6f).ok);
  TEST_ASSERT_FALSE(solveBridge(NAN, 0.1f, 480e3f, 2.409e6f).ok);
}

static void test_extrapolate_exponential() {
  const float tau = 2.0f;
  float v[3];
  for (int i = 0; i < 3; i++) v[i] = 0.8f - 0.5f * expf(-(float)(i + 1) / tau);
  float out = 0;
  TEST_ASSERT_TRUE(extrapolate(v[0], v[1], v[2], &out));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.8f, out);
  TEST_ASSERT_FALSE(extrapolate(0.1f, 0.3f, 0.2f, &out));   // turns back
  TEST_ASSERT_FALSE(extrapolate(0.1f, 0.2f, 0.35f, &out));  // accelerating
  TEST_ASSERT_FALSE(extrapolate(0.2f, 0.2f, 0.2f, &out));   // flat
}

// ------------------------------------------------------------------ MHS encoding

static void test_mhs_round_trip_through_decoder() {
  const float rs[] = {0.0f, 10.0f, 40.0f, 100.0f, 300.0f, 1200.0f, 10000.0f};
  for (float r : rs) {
    PwmCmd c = encodeMhs(MhsMode::Normal, r);
    TEST_ASSERT_EQUAL_UINT8(10, c.freqHz);
    ir155::Reading rd = ir155::decode((float)c.freqHz, c.dutyPct);
    TEST_ASSERT_TRUE(rd.state == ir155::State::Normal);
    TEST_ASSERT_FLOAT_WITHIN(fmaxf(0.01f, r * 0.001f), r, rd.rKohm);
  }
  TEST_ASSERT_TRUE(isinf(ir155::decode(10, kohmToDuty(INFINITY)).rKohm));
  TEST_ASSERT_TRUE(ir155::decode(20, encodeMhs(MhsMode::Undervoltage, 500).dutyPct).state ==
                   ir155::State::Undervoltage);
  PwmCmd c = encodeMhs(MhsMode::SpeedStartGood, 0);
  ir155::Reading rd = ir155::decode(c.freqHz, c.dutyPct);
  TEST_ASSERT_TRUE(rd.state == ir155::State::SpeedStart && rd.sstGood);
  c = encodeMhs(MhsMode::SpeedStartBad, 0);
  rd = ir155::decode(c.freqHz, c.dutyPct);
  TEST_ASSERT_TRUE(rd.state == ir155::State::SpeedStart && !rd.sstGood);
  c = encodeMhs(MhsMode::DeviceError, 0);
  TEST_ASSERT_TRUE(ir155::decode(c.freqHz, c.dutyPct).state == ir155::State::DeviceError);
  c = encodeMhs(MhsMode::EarthFault, 0);
  TEST_ASSERT_TRUE(ir155::decode(c.freqHz, c.dutyPct).state == ir155::State::EarthFault);
  TEST_ASSERT_EQUAL_UINT8(0, encodeMhs(MhsMode::Off, 0).freqHz);
}

// ------------------------------------------------------------------ Monitor

static void test_startup_gives_speed_start_then_normal() {
  Sim s;
  TEST_ASSERT_FALSE(s.out.okhs);
  s.run(1.5f);  // one short speed-start cycle (2 x 600 ms)
  TEST_ASSERT_EQUAL_UINT32(1, s.mon.status().cycles);
  TEST_ASSERT_TRUE(s.mon.status().mode == MhsMode::SpeedStartGood);
  TEST_ASSERT_FALSE_MESSAGE(s.out.okhs, "OKHS must stay low until a full measurement");
  s.run(10.0f);
  TEST_ASSERT_TRUE(s.mon.status().mode == MhsMode::Normal);
  TEST_ASSERT_TRUE(s.out.okhs);
}

static void test_healthy_vehicle_reads_correctly() {
  Sim s;
  s.run(20.0f);
  const Status& st = s.mon.status();
  TEST_ASSERT_TRUE(st.mode == MhsMode::Normal);
  assertRel(20e6f, st.rp, 0.10f);
  assertRel(20e6f, st.rn, 0.10f);
  TEST_ASSERT_TRUE(s.out.okhs);
  TEST_ASSERT_EQUAL_UINT8(10, s.out.mhs.freqHz);
}

static void test_fault_below_100_ohm_per_volt_drops_okhs() {
  Sim s;
  s.plant.rp = 30e3f;  // 75 ohm/V at 400 V
  s.run(20.0f);
  const Status& st = s.mon.status();
  TEST_ASSERT_TRUE(st.mode == MhsMode::Normal);  // a valid measurement, just a bad one
  assertRel(30e3f, st.riso, 0.05f);
  TEST_ASSERT_FALSE(s.out.okhs);
  // MHS still reports the value: decode it back.
  ir155::Reading rd = ir155::decode(s.out.mhs.freqHz, s.out.mhs.dutyPct);
  TEST_ASSERT_FLOAT_WITHIN(1.5f, 30.0f, rd.rKohm);
}

static void test_fault_appearing_while_running_trips_and_recovers() {
  Sim s;
  s.run(15.0f);
  TEST_ASSERT_TRUE(s.out.okhs);
  s.plant.rn = 20e3f;
  s.run(15.0f);
  TEST_ASSERT_FALSE(s.out.okhs);
  assertRel(20e3f, s.mon.status().rn, 0.05f);
  s.plant.rn = 20e6f;
  s.run(15.0f);
  TEST_ASSERT_TRUE(s.out.okhs);
}

static void test_broken_ke_wire_is_earth_fault() {
  Sim s;
  s.plant.keBroken = true;
  s.run(20.0f);
  TEST_ASSERT_TRUE(s.mon.status().mode == MhsMode::EarthFault);
  TEST_ASSERT_FALSE(s.out.okhs);
  TEST_ASSERT_EQUAL_UINT8(50, s.out.mhs.freqHz);
}

static void test_broken_e_wire_is_earth_fault_not_perfect_isolation() {
  Sim s;
  s.plant.eBroken = true;
  s.run(20.0f);
  TEST_ASSERT_TRUE(s.mon.status().mode == MhsMode::EarthFault);
  TEST_ASSERT_FALSE(s.out.okhs);
}

static void test_no_hv_is_device_error() {
  Sim s;
  s.plant.vbus = 0.0f;
  s.run(5.0f);
  TEST_ASSERT_TRUE(s.mon.status().mode == MhsMode::DeviceError);
  TEST_ASSERT_FALSE(s.out.okhs);
  TEST_ASSERT_EQUAL_UINT8(40, s.out.mhs.freqHz);
}

static void test_dead_r0_switch_is_device_error() {
  Sim s;
  s.plant.switchDead = true;
  s.run(30.0f);
  TEST_ASSERT_TRUE(s.mon.status().mode == MhsMode::DeviceError);
  TEST_ASSERT_FALSE(s.out.okhs);
}

static void test_inhibit_opens_both_switches_and_holds_state() {
  Sim s;
  s.run(15.0f);
  TEST_ASSERT_TRUE(s.out.okhs);
  s.inhibit = true;
  s.run(5.0f);
  TEST_ASSERT_FALSE(s.out.r0p);
  TEST_ASSERT_FALSE(s.out.r0n);
  TEST_ASSERT_TRUE(s.mon.status().phase == Phase::Inhibited);
  TEST_ASSERT_TRUE(s.out.okhs);  // held
  s.inhibit = false;
  s.run(15.0f);
  TEST_ASSERT_TRUE(s.mon.status().mode == MhsMode::Normal);
}

static void test_contactor_change_restarts_and_labels_scope() {
  Sim s;
  s.run(15.0f);
  TEST_ASSERT_FALSE(s.mon.status().systemScope);
  s.cont1 = true;
  s.run(0.04f);  // one sample
  TEST_ASSERT_TRUE(s.mon.status().phase == Phase::P);
  TEST_ASSERT_TRUE(s.out.r0p);
  s.run(15.0f);
  TEST_ASSERT_TRUE(s.mon.status().systemScope);
}

static void test_large_y_cap_still_converges() {
  Config c;
  c.maxPhaseMs = 8000;
  Sim s(c);
  s.plant.cy = 1e-6f;  // charger / DC-DC on the bus
  s.plant.rp = 1e6f;
  s.run(60.0f);
  TEST_ASSERT_TRUE(s.mon.status().mode == MhsMode::Normal);
  assertRel(1e6f, s.mon.status().rp, 0.10f);
}

static void test_noise_does_not_cause_false_trips() {
  Sim s;
  s.plant.noise = 0.5f;  // +/-0.5 V on every reading
  s.plant.rp = 2e6f;
  s.run(15.0f);
  for (int i = 0; i < 20; i++) {
    s.run(3.0f);
    TEST_ASSERT_TRUE(s.out.okhs);
    TEST_ASSERT_TRUE(s.mon.status().mode == MhsMode::Normal);
  }
  assertRel(2e6f, s.mon.status().rp, 0.25f);
}

// ------------------------------------------------------------------ pack-voltage transients

static void test_vbus_swings_hold_reading_then_go_stale() {
  Sim s;
  s.run(15.0f);
  TEST_ASSERT_TRUE(s.out.okhs);
  const uint32_t cycles = s.mon.status().cycles;
  s.setRipple(12.0f);          // +/-3 % square wave: hard acceleration / regen
  s.run(30.0f);
  const Status& st = s.mon.status();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(cycles, st.cycles, "no cycle may complete while VBUS swings 6 %");
  TEST_ASSERT_TRUE(st.transientRestarts > 0);
  TEST_ASSERT_TRUE_MESSAGE(s.out.okhs, "last valid reading is held");
  TEST_ASSERT_FALSE(st.stale);
  s.run(40.0f);                // 70 s without a valid measurement
  TEST_ASSERT_TRUE(s.mon.status().stale);
  TEST_ASSERT_TRUE_MESSAGE(s.mon.status().mode == MhsMode::Normal, "stale is a warning, not a fault");
  TEST_ASSERT_TRUE(s.out.okhs);
  s.setRipple(0);
  s.run(15.0f);
  TEST_ASSERT_FALSE(s.mon.status().stale);
  TEST_ASSERT_TRUE(s.mon.status().cycles > cycles);
}

static void test_small_ripple_finishes_in_relaxed_mode() {
  Sim s;
  s.plant.rp = 200e3f;
  s.setRipple(2.4f);           // +/-0.6 %: above the 0.5 % strict limit, inside the 2 % relaxed one
  s.run(40.0f);
  const Status& st = s.mon.status();
  TEST_ASSERT_TRUE(st.cycles >= 3);
  TEST_ASSERT_TRUE(st.lastRelaxed);
  TEST_ASSERT_TRUE(st.mode == MhsMode::Normal);
  assertRel(200e3f, st.rp, 0.10f);
}

static void test_steady_vbus_is_never_relaxed() {
  Sim s;
  s.plant.noise = 0.5f;
  s.run(20.0f);
  TEST_ASSERT_FALSE(s.mon.status().lastRelaxed);
  TEST_ASSERT_EQUAL_UINT32(0, s.mon.status().transientRestarts);
}

static void test_inhibit_does_not_go_stale() {
  Sim s;
  s.run(15.0f);
  s.inhibit = true;
  s.run(90.0f);
  TEST_ASSERT_FALSE(s.mon.status().stale);
}

// ------------------------------------------------------------------ calibration

// The "real" board for the calibration tests: R0, dividers and ADC gains all off nominal.
static void misbuild(Plant& p) {
  p.r0 = 492e3f;                 // nominal 480k
  p.rdivE = p.rdivKE = 4.70e6f;  // nominal 4.818M each
  p.err[0] = 1.012f; p.err[1] = 0.991f; p.err[2] = 1.006f;
}

static void feedGain(Plant& p, GainCal& g) {
  for (;;) {
    const Inputs in = p.read(false, false);  // both switches open
    if (g.add(in.vbus, in.ve, in.vke)) return;
  }
}

static void runGainCal(Sim& s, Calibration* cal, float vTrue = 250.0f) {
  s.plant.vbus = vTrue;
  s.plant.eTiedToHvp = true;
  GainCal g;
  g.start(vTrue);
  feedGain(s.plant, g);
  CalResult r = g.finish(cal);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.msg);
  s.plant.eTiedToHvp = false;
  s.corr[0] = cal->gVbus; s.corr[1] = cal->gVe; s.corr[2] = cal->gVke;
}

static CalResult runBridgeCal(Sim& s, Calibration* cal, float rcal) {
  BridgeCal b;
  b.start(rcal, Config());
  for (int i = 0; i < 20000 && !b.done(); i++) {
    Inputs in = s.sample();
    s.out = b.step(s.t, in);
  }
  TEST_ASSERT_TRUE_MESSAGE(b.done(), "bridge calibration did not finish");
  return b.finish(cal);
}

static void test_cal_crc_and_validation() {
  Calibration c;
  calSeal(&c);
  TEST_ASSERT_TRUE(calValid(c));
  Calibration bad = c;
  bad.r0 += 1.0f;              // changed without resealing
  TEST_ASSERT_FALSE(calValid(bad));
  bad = c;
  bad.gVe = 1.5f;              // sealed but absurd
  calSeal(&bad);
  TEST_ASSERT_FALSE(calValid(bad));
  Calibration blank;
  blank.magic = 0xFFFFFFFFu;   // erased flash
  TEST_ASSERT_FALSE(calValid(blank));
}

static void test_gain_cal_recovers_channel_errors() {
  Sim s;
  misbuild(s.plant);
  Calibration cal;
  runGainCal(s, &cal);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f / 1.012f, cal.gVbus);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f / 0.991f, cal.gVe);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f / 1.006f, cal.gVke);
  TEST_ASSERT_TRUE(cal.flags & Calibration::kGain);
  TEST_ASSERT_TRUE(calValid(cal));
}

static void test_gain_cal_rejects_wrong_wiring() {
  Sim s;
  s.plant.vbus = 250.0f;       // E/KE NOT tied to HV+: they read the chassis, ~0 V
  GainCal g;
  g.start(250.0f);
  feedGain(s.plant, g);
  Calibration cal;
  TEST_ASSERT_FALSE(g.finish(&cal).ok);
  g.start(300.0f);             // typed the wrong reference voltage (meter says 250)
  while (!g.add(250.0f, 250.0f, 250.0f)) {}
  TEST_ASSERT_FALSE(g.finish(&cal).ok);
}

static void test_solve_cal_matches_forward_model() {
  const float r0 = 495e3f, rdiv = 2.35e6f, rcal = 1e6f;
  const float fP = settledFraction(rcal, INFINITY, r0, rdiv, true, false);
  const float fN = settledFraction(rcal, INFINITY, r0, rdiv, false, true);
  float r0c, rdivc;
  TEST_ASSERT_TRUE(solveCal(fP, fN, rcal, &r0c, &rdivc));
  assertRel(r0, r0c, 1e-3f);
  assertRel(rdiv, rdivc, 1e-3f);
  TEST_ASSERT_FALSE(solveCal(0.3f, 0.0f, rcal, &r0c, &rdivc));
}

static void test_bridge_cal_end_to_end() {
  Sim s;
  misbuild(s.plant);
  Calibration cal;
  runGainCal(s, &cal);
  // Bench: 1 Mohm from HV+ to the floating E/KE node, nothing else, little capacitance.
  s.plant.vbus = 250.0f;
  s.plant.rp = 1e6f;
  s.plant.rn = INFINITY;
  s.plant.cy = 10e-9f;
  CalResult r = runBridgeCal(s, &cal, 1e6f);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.msg);
  assertRel(492e3f, cal.r0, 0.005f);
  assertRel(2.35e6f, cal.rdiv, 0.005f);
  TEST_ASSERT_TRUE(calValid(cal));

  // In the vehicle: a fault at 75 ohm/V on HV+, healthy HV-.
  const float rp = 30e3f, rn = 10e6f;
  Sim cald(applyCal(Config(), cal));
  misbuild(cald.plant);
  cald.corr[0] = cal.gVbus; cald.corr[1] = cal.gVe; cald.corr[2] = cal.gVke;
  cald.plant.rp = rp; cald.plant.rn = rn;
  cald.run(25.0f);
  Sim raw;                     // same hardware, no calibration
  misbuild(raw.plant);
  raw.plant.rp = rp; raw.plant.rn = rn;
  raw.run(25.0f);
  const float errCal = fabsf(cald.mon.status().rp / rp - 1.0f);
  const float errRaw = fabsf(raw.mon.status().rp / rp - 1.0f);
  char msg[96];
  snprintf(msg, sizeof msg, "Rp error: uncalibrated %.2f %%, calibrated %.3f %%", errRaw * 100, errCal * 100);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE_MESSAGE(errCal < 0.01f, msg);
  TEST_ASSERT_TRUE_MESSAGE(errCal < errRaw, msg);
  assertRel(rn, cald.mon.status().rn, 0.05f);
}

static void test_bridge_cal_rejects_leaky_chassis() {
  Sim s;
  s.plant.vbus = 250.0f;
  s.plant.rp = 1e6f;
  s.plant.rn = 500e3f;         // E/KE node touching the bench earth / HV- through something
  s.plant.cy = 10e-9f;
  Calibration cal;
  TEST_ASSERT_FALSE(runBridgeCal(s, &cal, 1e6f).ok);
}

static void test_bridge_cal_rejects_bad_setup() {
  Calibration cal;
  BridgeCal b;
  b.start(50e3f, Config());    // Rcal out of range
  TEST_ASSERT_TRUE(b.done());
  TEST_ASSERT_FALSE(b.finish(&cal).ok);
  Sim s;
  s.plant.vbus = 0.0f;         // no HV on J3
  TEST_ASSERT_FALSE(runBridgeCal(s, &cal, 1e6f).ok);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_solve_matches_forward_model);
  RUN_TEST(test_solve_rejects_identical_phases);
  RUN_TEST(test_extrapolate_exponential);
  RUN_TEST(test_mhs_round_trip_through_decoder);
  RUN_TEST(test_startup_gives_speed_start_then_normal);
  RUN_TEST(test_healthy_vehicle_reads_correctly);
  RUN_TEST(test_fault_below_100_ohm_per_volt_drops_okhs);
  RUN_TEST(test_fault_appearing_while_running_trips_and_recovers);
  RUN_TEST(test_broken_ke_wire_is_earth_fault);
  RUN_TEST(test_broken_e_wire_is_earth_fault_not_perfect_isolation);
  RUN_TEST(test_no_hv_is_device_error);
  RUN_TEST(test_dead_r0_switch_is_device_error);
  RUN_TEST(test_inhibit_opens_both_switches_and_holds_state);
  RUN_TEST(test_contactor_change_restarts_and_labels_scope);
  RUN_TEST(test_large_y_cap_still_converges);
  RUN_TEST(test_noise_does_not_cause_false_trips);
  RUN_TEST(test_vbus_swings_hold_reading_then_go_stale);
  RUN_TEST(test_small_ripple_finishes_in_relaxed_mode);
  RUN_TEST(test_steady_vbus_is_never_relaxed);
  RUN_TEST(test_inhibit_does_not_go_stale);
  RUN_TEST(test_cal_crc_and_validation);
  RUN_TEST(test_gain_cal_recovers_channel_errors);
  RUN_TEST(test_gain_cal_rejects_wrong_wiring);
  RUN_TEST(test_solve_cal_matches_forward_model);
  RUN_TEST(test_bridge_cal_end_to_end);
  RUN_TEST(test_bridge_cal_rejects_leaky_chassis);
  RUN_TEST(test_bridge_cal_rejects_bad_setup);
  return UNITY_END();
}
