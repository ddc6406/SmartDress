#include "sensors.h"
#include "pins.h"
#include <Wire.h>
#include <math.h>
#include <Adafruit_TCS34725.h>

namespace {
  constexpr float K_SERIES = 10000.0f;
  constexpr float BETA = 3950.0f;
  constexpr float R0 = 10000.0f;
  constexpr float T0K = 298.15f;
  constexpr float MV_SATURATED = 2800.0f;  // provisional; event-calibratable
  constexpr int MOISTURE_SAMPLES = 8;
  constexpr int TEMP_SAMPLES = 16;
  // 50 ms integration = 21 cycles, so the clear channel tops out at 1024*21 = 21504.
  constexpr uint16_t CLEAR_SATURATED = 20000;

  Adafruit_TCS34725 tcs(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);
  bool g_colorPresent = false;
  uint16_t g_lastClear = 0;

  float clampFloat(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
  }

  uint16_t avgMilliVolts(int pin, int samples) {
    uint32_t sum = 0;
    for (int i = 0; i < samples; i++) {
      sum += analogReadMilliVolts(pin);
      delayMicroseconds(200);
    }
    return static_cast<uint16_t>(sum / samples);
  }

  // RGB (0..1) → hue degrees 0..360
  float rgbToHue(float r, float g, float b) {
    float maxc = fmaxf(r, fmaxf(g, b));
    float minc = fminf(r, fminf(g, b));
    float delta = maxc - minc;
    if (delta < 1e-6f) return 0.0f;
    float hue;
    if (maxc == r) {
      hue = fmodf(((g - b) / delta), 6.0f);
    } else if (maxc == g) {
      hue = ((b - r) / delta) + 2.0f;
    } else {
      hue = ((r - g) / delta) + 4.0f;
    }
    hue *= 60.0f;
    if (hue < 0) hue += 360.0f;
    return hue;
  }

  // Provisional universal-indicator hue → pH (~4–9). Event-calibratable.
  float lookupPhFromHue(float hue) {
    struct Entry { float hueMax; float pH; };
    static const Entry table[] = {
      {20, 4.0f}, {40, 4.5f}, {60, 5.0f}, {80, 5.5f}, {100, 6.0f},
      {130, 6.5f}, {160, 7.0f}, {190, 7.5f}, {220, 8.0f}, {260, 8.5f},
      {300, 9.0f},
    };
    for (const auto& e : table) {
      if (hue < e.hueMax) return e.pH;
    }
    return NAN;
  }
}  // namespace

void sensorsInit() {
  Wire.begin(Pins::I2C_SDA, Pins::I2C_SCL);
  delay(50);
  g_colorPresent = tcs.begin();
  if (!g_colorPresent) {
    // Ensure QT / NeoPixel rail is powered, retry once
    digitalWrite(Pins::NEOPIXEL_POWER, HIGH);
    delay(100);
    g_colorPresent = tcs.begin();
  }
  if (g_colorPresent) {
    tcs.setInterrupt(true);  // on the STEMMA breakout the INT pin gates the LED: true = LED off
    Serial.println("[sensors] TCS34725 present");
  } else {
    Serial.println("[sensors] TCS34725 missing — pH estimate disabled");
  }
}

bool sensorsColorPresent() { return g_colorPresent; }

uint16_t readMoistureMv(int pin) {
  digitalWrite(Pins::EXC, HIGH);
  delay(5);  // ~5 ms pulse
  uint16_t mv = avgMilliVolts(pin, MOISTURE_SAMPLES);
  digitalWrite(Pins::EXC, LOW);
  return mv;
}

float moisturePercent(uint16_t mv, float dryBaselineMv) {
  float denom = MV_SATURATED - dryBaselineMv;
  if (fabsf(denom) < 1.0f) return 0.0f;
  return clampFloat((static_cast<float>(mv) - dryBaselineMv) / denom * 100.0f, 0.0f, 100.0f);
}

float readTempC(int pin) {
  uint16_t mv = avgMilliVolts(pin, TEMP_SAMPLES);
  if (mv <= 1 || mv >= 3290) return NAN;
  float rNtc = K_SERIES * static_cast<float>(mv) / (3300.0f - static_cast<float>(mv));
  float invT = (1.0f / T0K) + (logf(rNtc / R0) / BETA);
  return (1.0f / invT) - 273.15f;
}

float readPH() {
  if (!g_colorPresent) return NAN;
  tcs.setInterrupt(false);  // LED on
  delay(50);
  uint16_t r, g, b, c;
  tcs.getRawData(&r, &g, &b, &c);
  tcs.setInterrupt(true);   // LED off
  g_lastClear = c;
  if (c == 0 || c >= CLEAR_SATURATED) return NAN;  // no light, or saturated by ambient/LED glare
  float rf = static_cast<float>(r) / c;
  float gf = static_cast<float>(g) / c;
  float bf = static_cast<float>(b) / c;
  float hue = rgbToHue(rf, gf, bf);
  return lookupPhFromHue(hue);
}

uint16_t readLiftMv() { return avgMilliVolts(Pins::LIFT, 8); }

float liftNormalized(uint16_t mv, const SessionState& session) {
  float denom = session.liftMaxBendMv - session.liftFlatMv;
  if (fabsf(denom) < 1.0f) return 0.0f;
  return clampFloat((static_cast<float>(mv) - session.liftFlatMv) / denom, 0.0f, 1.0f);
}

void newDressingCalibration(SessionState& session) {
  Serial.println("[sensors] NEW_DRESSING calibration…");
  float offsetSum = 0.0f;
  int valid = 0;
  for (int i = 0; i < 8; i++) {
    float tw = readTempC(Pins::T_WOUND);
    float tr = readTempC(Pins::T_REF);
    if (!isnan(tw) && !isnan(tr)) {
      offsetSum += (tw - tr);
      valid++;
    }
    delay(200);
  }
  session.thermistorOffsetC = valid ? offsetSum / valid : 0.0f;
  session.baselineDryCenterMv = readMoistureMv(Pins::M_CENTER);
  session.baselineDryEdgeMv = readMoistureMv(Pins::M_EDGE);
  session.baselineColorPh = readPH();
  session.liftFlatMv = readLiftMv();
  // Provisional max bend span; refine at event with a peel calibration
  session.liftMaxBendMv = session.liftFlatMv + 800.0f;
  session.startMs = millis();
  session.wornMs = 0;
  session.baselineReady = false;
  session.baselineTempDiffC = 0.0f;
  session.active = true;
  Serial.printf("[sensors] dry C/E mV=%.0f/%.0f offset=%.2f C\n",
                session.baselineDryCenterMv, session.baselineDryEdgeMv,
                session.thermistorOffsetC);
}

bool maybeLockTempBaseline(SessionState& session, float tempDiff) {
  if (!session.active || session.baselineReady) return false;
  if (millis() - session.startMs < 20UL * 60UL * 1000UL) return false;
  session.baselineTempDiffC = tempDiff;
  session.baselineReady = true;
  Serial.printf("[sensors] temp baseline locked at %.2f C\n", tempDiff);
  return true;
}

uint8_t checkFaults(const SensorReadings& r) {
  uint8_t f = FAULT_NONE;
  auto atRail = [](uint16_t mv) { return mv < 20 || mv > 3250; };
  // readTempC() returns NAN for an open or shorted thermistor, so NAN is a fault too.
  auto tempBad = [](float c) { return isnan(c) || c < 15.0f || c > 50.0f; };
  if (atRail(r.centerMv) || atRail(r.edgeMv)) f |= FAULT_MOISTURE_OPEN;
  if (tempBad(r.tWoundC) || tempBad(r.tRefC)) f |= FAULT_TEMP_RANGE;
  if (!r.colorPresent) f |= FAULT_COLOR_MISSING;
  if (r.colorPresent && r.clearRaw >= CLEAR_SATURATED) f |= FAULT_AMBIENT_LIGHT;
  if (atRail(r.liftMv)) f |= FAULT_FLEX_OPEN;
  return f;
}

uint16_t sensorsLastClear() { return g_lastClear; }
