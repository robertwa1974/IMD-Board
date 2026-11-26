// Host tests for lib/imd_core/imd_proto (the IMD -> economizer UART frame).
// Run: pio test -e native
#include <unity.h>
#include <math.h>
#include <string.h>
#include "imd_proto.h"

using namespace imdproto;

void setUp() {}
void tearDown() {}

static Frame example() {
  Frame f;
  f.seq = 42;
  f.mode = Mode::Normal;
  f.ok = true;
  f.state = State::Ok;
  f.rpK = 20410;
  f.rnK = 19870;
  f.vbusDv = 3984;
  f.ageS = 0;
  f.flags = kSettled | kCalGain | kCalBridge | kHaveResult;  // 0x72
  return f;
}

static void test_golden_frame() {
  char buf[kMaxLine];
  const size_t n = format(example(), buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("$IMD,1,42,NORM,1,OK,20410,19870,3984,0,72*6F\r\n", buf);  // documented example
  TEST_ASSERT_EQUAL_UINT32(strlen(buf), n);
}

static void test_round_trip_all_codes_and_values() {
  const Mode modes[] = {Mode::Off, Mode::Normal, Mode::Undervoltage, Mode::SstGood, Mode::SstBad,
                        Mode::DeviceError, Mode::EarthFault};
  const State states[] = {State::Ok, State::Degraded, State::Interlock, State::Calibrating};
  const uint32_t values[] = {0, 1, 40, 99999, kOpen - 1, kOpen, kNoValue};
  for (Mode m : modes) {
    for (State s : states) {
      for (uint32_t v : values) {
        Frame f = example();
        f.mode = m; f.state = s; f.rpK = v; f.rnK = values[(v % 7)]; f.ok = (v & 1);
        f.seq = (uint8_t)(v * 7); f.ageS = v == kNoValue ? 0 : v % 100000; f.flags = 0xFF;
        char buf[kMaxLine];
        TEST_ASSERT_TRUE(format(f, buf, sizeof buf) > 0);
        Frame g;
        TEST_ASSERT_TRUE_MESSAGE(parse(buf, &g), buf);
        TEST_ASSERT_TRUE(g.mode == f.mode && g.state == f.state && g.ok == f.ok);
        TEST_ASSERT_EQUAL_UINT32(f.rpK, g.rpK);
        TEST_ASSERT_EQUAL_UINT32(f.rnK, g.rnK);
        TEST_ASSERT_EQUAL_UINT32(f.seq, g.seq);
        TEST_ASSERT_EQUAL_UINT32(f.ageS, g.ageS);
        TEST_ASSERT_EQUAL_UINT8(f.flags, g.flags);
      }
    }
  }
}

static void test_worst_case_length_fits() {
  Frame f;
  f.seq = 255; f.mode = Mode::EarthFault; f.state = State::Calibrating;
  f.rpK = kOpen - 1; f.rnK = kOpen - 1; f.vbusDv = 99999; f.ageS = 0xFFFFFFFFu; f.flags = 0xFF;
  char buf[kMaxLine];
  TEST_ASSERT_TRUE(format(f, buf, sizeof buf) > 0);
  TEST_ASSERT_EQUAL_UINT32(0, format(f, buf, 20));  // too small: refused, not truncated
}

static void test_rejects_corruption() {
  char buf[kMaxLine];
  format(example(), buf, sizeof buf);
  Frame g;
  TEST_ASSERT_TRUE(parse(buf, &g));
  char bad[kMaxLine];
  // every single-character change in the body must be caught by the checksum or the syntax
  const char* star = strchr(buf, '*');
  for (const char* c = buf + 1; c < star; c++) {
    strcpy(bad, buf);
    bad[c - buf] ^= 0x01;
    TEST_ASSERT_FALSE_MESSAGE(parse(bad, &g), bad);
  }
  TEST_ASSERT_FALSE(parse("$IMD,1,42,NORM,1,OK,20410,19870,3984,0,72*6E", &g));  // wrong checksum
  TEST_ASSERT_FALSE(parse("$IMD,1,42,NORM,1,OK,20410,19870,3984,0,72", &g));     // no checksum
  TEST_ASSERT_FALSE(parse("$IMD,1,42,NORM,1,OK,20410,19870,3984,0,72*6", &g));   // short checksum
  TEST_ASSERT_FALSE(parse("$IMD,1,42,NORM,1,OK,20410,19870,3984,0,72*6Fx", &g)); // junk after
  TEST_ASSERT_FALSE(parse("# IMD mode=NORMAL", &g));                             // human line
  TEST_ASSERT_FALSE(parse("CAL OK: gain calibration stored", &g));               // reply text
  TEST_ASSERT_FALSE(parse("", &g));
  TEST_ASSERT_FALSE(parse(nullptr, &g));
}

// Builds "$<body>*<cs>" with a correct checksum, to test the field checks on their own.
static const char* withCs(const char* body) {
  static char out[kMaxLine + 8];
  uint8_t cs = 0;
  for (const char* c = body; *c; c++) cs ^= (uint8_t)*c;
  const char hex[] = "0123456789ABCDEF";
  out[0] = '$';
  strcpy(out + 1, body);
  size_t n = strlen(out);
  out[n++] = '*'; out[n++] = hex[cs >> 4]; out[n++] = hex[cs & 15]; out[n] = 0;
  return out;
}

static void test_rejects_bad_fields_even_with_valid_checksum() {
  Frame g;
  TEST_ASSERT_TRUE(parse(withCs("IMD,1,42,NORM,1,OK,20410,19870,3984,0,72"), &g));
  TEST_ASSERT_TRUE(parse(withCs("IMD,1,0,DEV,0,CAL,-,-,0,0,00"), &g));
  TEST_ASSERT_TRUE(parse(withCs("IMD,1,0,NORM,1,OK,INF,INF,4000,3,40"), &g));
  TEST_ASSERT_FALSE(parse(withCs("IMD,2,42,NORM,1,OK,20410,19870,3984,0,72"), &g));      // version
  TEST_ASSERT_FALSE(parse(withCs("IMD,1,256,NORM,1,OK,20410,19870,3984,0,72"), &g));    // seq range
  TEST_ASSERT_FALSE(parse(withCs("IMD,1,42,NORMAL,1,OK,20410,19870,3984,0,72"), &g));   // mode code
  TEST_ASSERT_FALSE(parse(withCs("IMD,1,42,NORM,2,OK,20410,19870,3984,0,72"), &g));     // ok flag
  TEST_ASSERT_FALSE(parse(withCs("IMD,1,42,NORM,1,FINE,20410,19870,3984,0,72"), &g));   // state
  TEST_ASSERT_FALSE(parse(withCs("IMD,1,42,NORM,1,OK,-5,19870,3984,0,72"), &g));        // negative
  TEST_ASSERT_FALSE(parse(withCs("IMD,1,42,NORM,1,OK,20410,19870,3984,0"), &g));        // 10 fields
  TEST_ASSERT_FALSE(parse(withCs("IMD,1,42,NORM,1,OK,20410,19870,3984,0,72,9"), &g));   // 12 fields
  TEST_ASSERT_FALSE(parse(withCs("IMD,1,42,NORM,1,OK,20410,19870,3984,0,7"), &g));      // 1 flag digit
  TEST_ASSERT_FALSE(parse(withCs("IMX,1,42,NORM,1,OK,20410,19870,3984,0,72"), &g));     // talker
  TEST_ASSERT_FALSE(parse(withCs("IMD,1,42,NORM,1,OK,,19870,3984,0,72"), &g));          // empty
}

static void test_ohms_to_field() {
  TEST_ASSERT_EQUAL_UINT32(kOpen, ohmsToField(INFINITY));
  TEST_ASSERT_EQUAL_UINT32(kNoValue, ohmsToField(NAN));
  TEST_ASSERT_EQUAL_UINT32(kNoValue, ohmsToField(-1.0f));
  TEST_ASSERT_EQUAL_UINT32(40, ohmsToField(40e3f));
  TEST_ASSERT_EQUAL_UINT32(20000, ohmsToField(20e6f));
  TEST_ASSERT_EQUAL_UINT32(0, ohmsToField(400.0f));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_golden_frame);
  RUN_TEST(test_round_trip_all_codes_and_values);
  RUN_TEST(test_worst_case_length_fits);
  RUN_TEST(test_rejects_corruption);
  RUN_TEST(test_rejects_bad_fields_even_with_valid_checksum);
  RUN_TEST(test_ohms_to_field);
  return UNITY_END();
}
