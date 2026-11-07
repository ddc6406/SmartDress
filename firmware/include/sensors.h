#pragma once

#include <Arduino.h>

struct SensorReadings {
  float centerPct;
  float edgePct;
  float tempDiffC;
  float pHEst;
  float lift;
  float hoursWorn;
  uint8_t faults;       // bitfield — see FaultFlags
  bool colorPresent;
  float tWoundC;
  float tRefC;
  uint16_t centerMv;
  uint16_t edgeMv;
  uint16_t liftMv;
  uint16_t clearRaw;    // TCS34725 clear channel from the last pH read
};

enum FaultFlags : uint8_t {
  FAULT_NONE = 0,
  FAULT_MOISTURE_OPEN = 1 << 0,
  FAULT_TEMP_RANGE = 1 << 1,
  FAULT_COLOR_MISSING = 1 << 2,
  FAULT_AMBIENT_LIGHT = 1 << 3,
  FAULT_FLEX_OPEN = 1 << 4,
};

// Faults that make the pod's moisture/temp/lift readings untrustworthy. Colour-sensor
// faults only disable the pH estimate, so they must not suppress the other alerts.
constexpr uint8_t FAULTS_BLOCKING = FAULT_MOISTURE_OPEN | FAULT_TEMP_RANGE | FAULT_FLEX_OPEN;

struct SessionState {
  uint32_t startMs;          // millis() at apply; rebuilt from wornMs after a reboot
  uint32_t wornMs;           // elapsed wear, persisted periodically (millis() resets on boot)
  float baselineDryCenterMv;
  float baselineDryEdgeMv;
  float thermistorOffsetC;
  float baselineTempDiffC;
  float baselineColorPh;
  float liftFlatMv;
  float liftMaxBendMv;
  bool baselineReady;
  bool active;
};

void sensorsInit();
bool sensorsColorPresent();

uint16_t readMoistureMv(int pin);
float moisturePercent(uint16_t mv, float dryBaselineMv);
float readTempC(int pin);
float readPH();
uint16_t readLiftMv();
float liftNormalized(uint16_t mv, const SessionState& session);

void newDressingCalibration(SessionState& session);
bool maybeLockTempBaseline(SessionState& session, float tempDiff);
uint8_t checkFaults(const SensorReadings& r);
uint16_t sensorsLastClear();  // clear channel from the most recent readPH()
