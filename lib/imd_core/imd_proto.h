// IMD -> economizer status frame over the stacking-header UART (no hardware dependencies).
//
// This file and imd_proto.cpp are the source of truth; the economizer repo carries a copy in
// lib/imd_proto/ - change both together (bump kVersion if the field list changes).
//
// The UART carries INFORMATION only. The safety path stays hard-wired (IMD_OK, IMD_INHIBIT,
// CONTx_CLOSED), so a dead, noisy or misconfigured UART can never block or open a contactor.
//
// 115200 8N1, 3.3 V, one ASCII line per frame, NMEA style, every 500 ms:
//
//   $IMD,<ver>,<seq>,<mode>,<ok>,<state>,<rp>,<rn>,<vbus>,<age>,<flags>*<cs>\r\n
//
//   ver    protocol version, 1
//   seq    0..255, +1 per frame (gaps = lost frames)
//   mode   what MHS shows: OFF NORM UV SSTG SSTB DEV EARTH
//   ok     OKHS / IMD_OK level as driven: 0 / 1
//   state  OK, DEG (no valid measurement for 60 s, last result held), INT (R0 interlock tripped),
//          CAL (calibrating, not monitoring)
//   rp rn  insulation HV+ / HV- to chassis in kOhm (integer), INF = no measurable leakage,
//          - = no result yet
//   vbus   pack voltage in 0.1 V
//   age    seconds since the last valid measurement
//   flags  2 hex digits: 01 system scope (a contactor was closed), 02 last cycle settled,
//          04 last cycle relaxed (VBUS ripple), 08 inhibited, 10 gain-calibrated,
//          20 bridge-calibrated, 40 has a result
//   cs     XOR of every character between '$' and '*', 2 hex digits
//
// Example: $IMD,1,42,NORM,1,OK,20410,19870,3984,0,72*6F
//          (normal, OK, Rp 20.4 MOhm, Rn 19.9 MOhm, 398.4 V, fresh, settled + both calibrations + result)
// Other lines on the same UART: replies to commands (plain text) and the human status line
// (starts with '#'). A receiver only acts on lines that start with "$IMD," and pass the checksum.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace imdproto {

constexpr uint8_t kVersion = 1;
constexpr uint32_t kNoValue = 0xFFFFFFFFu;  // "-"   no result yet
constexpr uint32_t kOpen = 0xFFFFFFFEu;     // "INF" no measurable leakage
constexpr size_t kMaxLine = 96;             // longest frame incl. "\r\n" and terminator

enum class Mode : uint8_t { Off, Normal, Undervoltage, SstGood, SstBad, DeviceError, EarthFault };
enum class State : uint8_t { Ok, Degraded, Interlock, Calibrating };

enum Flags : uint8_t {
  kSystemScope = 0x01,
  kSettled = 0x02,
  kRelaxed = 0x04,
  kInhibited = 0x08,
  kCalGain = 0x10,
  kCalBridge = 0x20,
  kHaveResult = 0x40,
};

struct Frame {
  uint8_t seq = 0;
  Mode mode = Mode::Off;
  bool ok = false;
  State state = State::Ok;
  uint32_t rpK = kNoValue;   // kOhm, or kOpen / kNoValue
  uint32_t rnK = kNoValue;
  uint32_t vbusDv = 0;       // 0.1 V
  uint32_t ageS = 0;
  uint8_t flags = 0;
};

const char* modeCode(Mode m);
const char* stateCode(State s);

// Ohms -> the kOhm field value (rounded; INFINITY or > 4e12 -> kOpen; NaN / negative -> kNoValue).
uint32_t ohmsToField(float ohms);

// Writes the frame including the trailing "\r\n" and a terminating 0. Returns the length without
// the terminator, or 0 if the buffer is too small (kMaxLine is always enough).
size_t format(const Frame& f, char* buf, size_t size);

// Parses one line (with or without the trailing "\r\n"). Strict: "$IMD,", version 1, exactly
// eleven fields, valid numbers and codes, and a matching checksum. Returns false otherwise.
bool parse(const char* line, Frame* out);

}  // namespace imdproto
