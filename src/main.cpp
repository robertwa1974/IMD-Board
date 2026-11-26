// Example: read IR155 MHS + OKHS on an M5Dial (ESP32-S3) and print to Serial.
// Pins: M5Dial Port B is G1/G2 -- confirm against your board revision.
#include <Arduino.h>
#include "ir155_decoder.h"

constexpr uint8_t PIN_MHS = 1;
constexpr uint8_t PIN_OKHS = 2;
constexpr bool INPUT_INVERTED = true;  // true for the N-MOSFET + pull-up input stage

// Your own trip/warn levels, independent of the IR155's internal Ran (100 kOhm default)
constexpr float WARN_KOHM = 500.0f;
constexpr float FAULT_KOHM = 100.0f;

ir155::Decoder imd;

void setup() {
  Serial.begin(115200);
  imd.begin(PIN_MHS, INPUT_INVERTED);
  pinMode(PIN_OKHS, INPUT);
}

void loop() {
  ir155::Reading r = imd.update();  // call often; it's non-blocking

  static uint32_t tPrint = 0;
  if (millis() - tPrint < 500) return;
  tPrint = millis();

  bool okhs = digitalRead(PIN_OKHS) ^ INPUT_INVERTED;

  Serial.printf("%-12s f=%5.1fHz duty=%5.1f%% ", ir155::stateName(r.state), r.freqHz, r.dutyPct);
  if (r.state == ir155::State::Normal || r.state == ir155::State::Undervoltage) {
    if (isinf(r.rKohm)) Serial.print("R=open ");
    else Serial.printf("R=%.0fk ", r.rKohm);
    if (r.rKohm < FAULT_KOHM) Serial.print("[FAULT] ");
    else if (r.rKohm < WARN_KOHM) Serial.print("[WARN] ");
  } else if (r.state == ir155::State::SpeedStart) {
    Serial.print(r.sstGood ? "SST=good " : "SST=bad ");
  } else if (r.state == ir155::State::NoSignal) {
    Serial.printf("line stuck %s ", r.lineHigh ? "HIGH" : "LOW");
  }
  Serial.printf("OKHS=%s\n", okhs ? "OK" : "NOT OK");
}
