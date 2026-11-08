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
  uint16_t tWoundMv;    // raw thermistor node voltages (for the live dashboard)
  uint16_t tRefMv;
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

uint16_t readPinMv(int pin);  // averaged, for bench diagnostics
uint16_t readMoistureMv(int pin);
float moisturePercent(uint16_t mv, float dryBaselineMv, float wetMv);
float readTempC(int pin);
float tempFromMv(uint16_t mv);  // NTC divider mV -> °C (NAN if open/shorted)
float readPH();  // reads the strip under the colour sensor; NAN if no chart match / no white ref

// Last colour-sensor reading, for the live dashboard.
struct ColorReading {
  uint16_t r, g, b, c;  // raw TCS34725 counts with the sensor LED on
  float pH;             // trusted pH: calibrated AND within the match radius (NAN otherwise). Used for alerts.
  float nearestPh;      // best-guess pH (closest point on the chart path), always shown on the dashboard
  float matchDist;      // colour points off the chart path (NAN = not computed)
  bool whiteSet;        // a white reference has been captured (otherwise a typical default is used)
};
const ColorReading& lastColor();

// Leniency: how far (in colour points) a reading may sit off the chart's colour path and still
// count as a strip colour. In-between strip colours lie on the path (~0 points); white paper is
// ~20 points off and skin ~10, so 8 accepts real strips and rejects things that aren't strips.
constexpr float PH_MATCH_RADIUS = 8.0f;  // default; adjustable at runtime
void setPhMatchRadius(float points);
// Pure matcher (testable): raw sensor r,g,b + the white reference -> pH, or NAN if off-chart.
// nearestOut (optional) always receives the closest chart pH, even when off-chart.
float matchChartPh(float r, float g, float b, const float white[3], float* distOut,
                   float* nearestOut = nullptr);
// White reference: hold the sensor over plain white paper. Returns false if too dark,
// saturated or no sensor. On success writes the raw r,g,b to out and starts using them.
bool captureWhiteRef(uint16_t out[3]);
void setWhiteRef(const uint16_t rgb[3]);
uint16_t readLiftMv();
float liftNormalized(uint16_t mv, const SessionState& session, float fullScaleMv);

void newDressingCalibration(SessionState& session);
bool maybeLockTempBaseline(SessionState& session, float tempDiff);
uint8_t checkFaults(const SensorReadings& r);
uint16_t sensorsLastClear();  // clear channel from the most recent readPH()
