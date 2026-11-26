// Host unit tests for the IR155 MHS decode logic (no hardware).
// Run: pio test -e native     (needs a host C++ compiler on PATH, e.g. MinGW-w64 gcc)
#include <unity.h>
#include <cmath>
#include "ir155_decoder.h"

using namespace ir155;

void setUp() {}
void tearDown() {}

static void test_duty_to_kohm() {
  TEST_ASSERT_TRUE(std::isinf(dutyToKohm(5.0f)));
  TEST_ASSERT_TRUE(std::isinf(dutyToKohm(2.0f)));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, dutyToKohm(95.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 1200.0f, dutyToKohm(50.0f));  // 108000/45 - 1200
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 300.0f, dutyToKohm(77.0f));   // 108000/72 - 1200
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 133.3f, dutyToKohm(86.0f));
}

static void test_normal_and_undervoltage() {
  Reading r = decode(10.2f, 50.0f);
  TEST_ASSERT_TRUE(r.state == State::Normal);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 1200.0f, r.rKohm);
  TEST_ASSERT_TRUE(decode(19.5f, 30.0f).state == State::Undervoltage);
  TEST_ASSERT_TRUE(decode(10.0f, 99.0f).state == State::Invalid);
}

static void test_speed_start() {
  Reading r = decode(30.0f, 7.0f);
  TEST_ASSERT_TRUE(r.state == State::SpeedStart && r.sstGood);
  r = decode(30.0f, 92.0f);
  TEST_ASSERT_TRUE(r.state == State::SpeedStart && !r.sstGood);
  TEST_ASSERT_TRUE(decode(30.0f, 50.0f).state == State::Invalid);
}

static void test_error_states() {
  TEST_ASSERT_TRUE(decode(40.5f, 50.0f).state == State::DeviceError);
  TEST_ASSERT_TRUE(decode(49.0f, 50.0f).state == State::EarthFault);
}

static void test_bad_frequencies() {
  TEST_ASSERT_TRUE(decode(15.0f, 50.0f).state == State::Invalid);
  TEST_ASSERT_TRUE(decode(60.0f, 50.0f).state == State::Invalid);
  TEST_ASSERT_TRUE(decode(5.0f, 50.0f).state == State::Invalid);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_duty_to_kohm);
  RUN_TEST(test_normal_and_undervoltage);
  RUN_TEST(test_speed_start);
  RUN_TEST(test_error_states);
  RUN_TEST(test_bad_frequencies);
  return UNITY_END();
}
