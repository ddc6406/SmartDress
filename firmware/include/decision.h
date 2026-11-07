#pragma once

#include <Arduino.h>
#include "sensors.h"

// Urgency order (most urgent wins) — Appendix A
enum class Urgency : uint8_t {
  OK = 0,
  CHANGE_SOON = 1,
  CHANGE_NOW = 2,
  CONTACT_CLINIC = 3,
  SENSOR_FAULT = 4,
};

// Soft advisory reason codes for BLE + app plain-language banners
enum ReasonCode : uint8_t {
  REASON_NONE = 0,
  REASON_TEMP_SUSTAINED = 1,
  REASON_PH_SUSTAINED = 2,
  REASON_EDGE_WET = 3,
  REASON_SATURATED = 4,
  REASON_LIFT = 5,
  REASON_MAX_WEAR = 6,
  REASON_SATURATION_SOON = 7,
  REASON_SENSOR_FAULT = 8,
};

struct Thresholds {
  float saturationSoonPct;
  float saturationNowPct;
  float edgeWetPct;
  float tempDiffRiseC;
  uint16_t tempSustainMin;
  float pHAlert;
  uint16_t pHSustainMin;
  uint16_t maxWearHours;
  float liftThreshold;
};

struct DecisionTimers {
  uint32_t tempOverSinceMs;  // 0 = not over
  uint32_t pHOverSinceMs;
};

struct DecisionResult {
  Urgency urgency;
  uint8_t reasonCount;
  uint8_t reasons[4];  // ReasonCode values
};

Thresholds defaultThresholds();
bool thresholdsValid(const Thresholds& t);  // same ranges the web form enforces
DecisionResult evaluateStatus(const SensorReadings& r, const Thresholds& t,
                              const SessionState& session, DecisionTimers& timers);
const char* urgencyName(Urgency u);
const char* reasonPlainLanguage(uint8_t code);
