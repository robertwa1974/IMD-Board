#include "imd_proto.h"
#include <math.h>
#include <string.h>

namespace imdproto {

namespace {

const char* const kModes[] = {"OFF", "NORM", "UV", "SSTG", "SSTB", "DEV", "EARTH"};
const char* const kStates[] = {"OK", "DEG", "INT", "CAL"};
const char kHex[] = "0123456789ABCDEF";

// Tiny append-only writer: no snprintf (it costs flash on the 64 kB parts at both ends).
struct Out {
  char* p;
  char* end;
  bool ok = true;
  void ch(char c) {
    if (p < end) *p++ = c;
    else ok = false;
  }
  void str(const char* s) {
    while (*s) ch(*s++);
  }
  void num(uint32_t v) {
    char tmp[10];
    int n = 0;
    do {
      tmp[n++] = (char)('0' + v % 10);
      v /= 10;
    } while (v);
    while (n) ch(tmp[--n]);
  }
  void hex2(uint8_t v) {
    ch(kHex[v >> 4]);
    ch(kHex[v & 15]);
  }
  void value(uint32_t v) {
    if (v == kNoValue) ch('-');
    else if (v == kOpen) str("INF");
    else num(v);
  }
};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool parseUint(const char* s, uint32_t max, uint32_t* out) {
  if (!*s) return false;
  uint64_t v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return false;
    v = v * 10 + (uint64_t)(*s - '0');
    if (v > max) return false;
  }
  *out = (uint32_t)v;
  return true;
}

bool parseValue(const char* s, uint32_t* out) {
  if (!strcmp(s, "-")) { *out = kNoValue; return true; }
  if (!strcmp(s, "INF")) { *out = kOpen; return true; }
  return parseUint(s, kOpen - 1, out);
}

template <typename E, size_t N>
bool parseCode(const char* s, const char* const (&codes)[N], E* out) {
  for (size_t i = 0; i < N; i++) {
    if (!strcmp(s, codes[i])) { *out = (E)i; return true; }
  }
  return false;
}

}  // namespace

const char* modeCode(Mode m) {
  const size_t i = (size_t)m;
  return i < sizeof kModes / sizeof kModes[0] ? kModes[i] : "?";
}

const char* stateCode(State s) {
  const size_t i = (size_t)s;
  return i < sizeof kStates / sizeof kStates[0] ? kStates[i] : "?";
}

uint32_t ohmsToField(float ohms) {
  if (isnan(ohms) || ohms < 0.0f) return kNoValue;
  if (isinf(ohms) || ohms > 4.0e12f) return kOpen;
  return (uint32_t)(ohms / 1000.0f + 0.5f);
}

size_t format(const Frame& f, char* buf, size_t size) {
  if (size < 2) return 0;
  Out o{buf, buf + size - 1};
  o.str("$IMD,");
  o.num(kVersion); o.ch(',');
  o.num(f.seq); o.ch(',');
  o.str(modeCode(f.mode)); o.ch(',');
  o.ch(f.ok ? '1' : '0'); o.ch(',');
  o.str(stateCode(f.state)); o.ch(',');
  o.value(f.rpK); o.ch(',');
  o.value(f.rnK); o.ch(',');
  o.num(f.vbusDv); o.ch(',');
  o.num(f.ageS); o.ch(',');
  o.hex2(f.flags);
  if (!o.ok) return 0;
  uint8_t cs = 0;
  for (const char* c = buf + 1; c < o.p; c++) cs ^= (uint8_t)*c;
  o.ch('*');
  o.hex2(cs);
  o.str("\r\n");
  if (!o.ok) return 0;
  *o.p = 0;
  return (size_t)(o.p - buf);
}

bool parse(const char* line, Frame* out) {
  if (!line || strncmp(line, "$IMD,", 5) != 0) return false;
  const char* star = strchr(line, '*');
  if (!star || star - line > (ptrdiff_t)kMaxLine) return false;
  // Checksum: two hex digits, then only an optional "\r\n".
  const int h = hexDigit(star[1]), l = h < 0 ? -1 : hexDigit(star[2]);
  if (h < 0 || l < 0) return false;
  const char* tail = star + 3;
  while (*tail == '\r' || *tail == '\n') tail++;
  if (*tail) return false;
  uint8_t cs = 0;
  for (const char* c = line + 1; c < star; c++) cs ^= (uint8_t)*c;
  if (cs != (uint8_t)(h * 16 + l)) return false;

  // Split the body into fields.
  char body[kMaxLine];
  const size_t n = (size_t)(star - line - 1);
  memcpy(body, line + 1, n);
  body[n] = 0;
  const char* field[12];
  size_t count = 0;
  char* p = body;
  for (;;) {
    if (count == 12) return false;      // too many fields
    field[count++] = p;
    char* comma = strchr(p, ',');
    if (!comma) break;
    *comma = 0;
    p = comma + 1;
  }
  if (count != 11 || strcmp(field[0], "IMD") != 0) return false;

  Frame f;
  uint32_t v;
  if (!parseUint(field[1], 255, &v) || v != kVersion) return false;
  if (!parseUint(field[2], 255, &v)) return false;
  f.seq = (uint8_t)v;
  if (!parseCode(field[3], kModes, &f.mode)) return false;
  if (!strcmp(field[4], "1")) f.ok = true;
  else if (strcmp(field[4], "0") != 0) return false;
  if (!parseCode(field[5], kStates, &f.state)) return false;
  if (!parseValue(field[6], &f.rpK) || !parseValue(field[7], &f.rnK)) return false;
  if (!parseUint(field[8], 99999, &f.vbusDv)) return false;
  if (!parseUint(field[9], 0xFFFFFFFFu, &f.ageS)) return false;
  if (strlen(field[10]) != 2) return false;
  const int fh = hexDigit(field[10][0]), fl = hexDigit(field[10][1]);
  if (fh < 0 || fl < 0) return false;
  f.flags = (uint8_t)(fh * 16 + fl);
  *out = f;
  return true;
}

}  // namespace imdproto
