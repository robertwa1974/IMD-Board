// Host tests for lib/imd_core: bridge maths, MHS encoding (round trip through the IR155
// decoder), and the Monitor state machine against a simulated vehicle.
// Run: pio test -e native
#include <unity.h>
#include <math.h>
#include <stdint.h>
#include "imd_bridge.h"
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
  float r0 = 500e3f;
  float rdivE = 5.018e6f, rdivKE = 5.018e6f;
  bool eBroken = false, keBroken = false, switchDead = false;
  float noise = 0.0f;           // +/- volts of pseudo-random noise on every reading
  float vch = 0.0f;             // chassis voltage
  uint32_t seed = 12345;

  static float g(float r) { return isinf(r) ? 0.0f : 1.0f / r; }

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
  explicit Sim(const Config& c = Config()) : mon(c) {}
  void run(float seconds) {
    const uint32_t steps = (uint32_t)(seconds * 1000.0f / 35.0f);
    for (uint32_t i = 0; i < steps; i++) {
      plant.advance(0.035f, out.r0p, out.r0n);
      Inputs in = plant.read(out.r0p, out.r0n);
      in.cont1 = cont1;
      in.inhibit = inhibit;
      t += 35;
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
  const float r0 = 500e3f, rdiv = 2.509e6f;
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
  TEST_ASSERT_FALSE(solveBridge(0.3f, 0.3f, 500e3f, 2.509e6f).ok);
  TEST_ASSERT_FALSE(solveBridge(NAN, 0.1f, 500e3f, 2.509e6f).ok);
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
  return UNITY_END();
}
