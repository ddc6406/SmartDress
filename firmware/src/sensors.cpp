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
  constexpr int MOISTURE_SAMPLES = 8;
  constexpr int TEMP_SAMPLES = 16;
  constexpr float LIFT_FULL_SCALE_MV = 300.0f;
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
    // The ADC's sample-and-hold still carries charge from the previously read channel, which
    // skews the first conversion on a high-impedance pin (e.g. the flex divider). Discard it.
    analogReadMilliVolts(pin);
    delayMicroseconds(200);
    for (int i = 0; i < samples; i++) {
      sum += analogReadMilliVolts(pin);
      delayMicroseconds(200);
    }
    return static_cast<uint16_t>(sum / samples);
  }

  // Reference chart (the team's pH 1-14 strip box), sampled from a photo and white-balanced
  // against the chart's own white paper. r/g/b are chromaticity (colour independent of
  // brightness, sums to 1); lum is brightness relative to that white.
  struct ChartRef { float pH, r, g, b, lum; };
  const ChartRef kChart[] = {
    {1, 0.534f, 0.205f, 0.260f, 0.36f},  {2, 0.564f, 0.232f, 0.204f, 0.40f},
    {3, 0.607f, 0.259f, 0.134f, 0.42f},  {4, 0.548f, 0.304f, 0.148f, 0.49f},
    {5, 0.539f, 0.357f, 0.104f, 0.54f},  {6, 0.498f, 0.385f, 0.117f, 0.62f},
    {7, 0.486f, 0.413f, 0.101f, 0.50f},  {8, 0.397f, 0.381f, 0.222f, 0.47f},
    {9, 0.420f, 0.400f, 0.180f, 0.35f},  {10, 0.342f, 0.393f, 0.265f, 0.19f},
    {11, 0.277f, 0.345f, 0.379f, 0.11f}, {12, 0.291f, 0.274f, 0.435f, 0.19f},
    {13, 0.402f, 0.237f, 0.361f, 0.12f}, {14, 0.377f, 0.272f, 0.351f, 0.15f},
  };
  // Tuned on the chart: 20 keeps pH 6-9 within +/-0.5 for both +/-15% brightness drift and
  // +/-2-point colour noise, and every in-between colour reads between its own two squares.
  constexpr float BRIGHTNESS_WEIGHT = 20.0f;
  constexpr uint16_t CLEAR_TOO_DARK = 200;    // nothing under the sensor / LED not reflecting

  float g_white[3] = {0, 0, 0};
  float g_phRadius = PH_MATCH_RADIUS;
  // Used only for the uncalibrated best guess: what this Feather's TCS34725 read over a whitish
  // surface with its LED on (green-heavy, as is typical for this sensor).
  const float kDefaultWhite[3] = {1019, 1228, 1027};
  ColorReading g_color{0, 0, 0, 0, NAN, NAN, NAN, false};

  bool readColorRaw(uint16_t& r, uint16_t& g, uint16_t& b, uint16_t& c) {
    if (!g_colorPresent) return false;
    tcs.setInterrupt(false);  // LED on
    delay(120);               // > 2 integration cycles, so the sample is fully LED-lit
    tcs.getRawData(&r, &g, &b, &c);
    tcs.setInterrupt(true);   // LED off
    g_lastClear = c;
    return true;
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

uint16_t readPinMv(int pin) { return avgMilliVolts(pin, 16); }

uint16_t readMoistureMv(int pin) {
  digitalWrite(Pins::EXC, HIGH);
  delay(5);  // ~5 ms pulse
  uint16_t mv = avgMilliVolts(pin, MOISTURE_SAMPLES);
  digitalWrite(Pins::EXC, LOW);
  return mv;
}

float moisturePercent(uint16_t mv, float dryBaselineMv, float wetMv) {
  float denom = wetMv - dryBaselineMv;
  if (fabsf(denom) < 1.0f) return 0.0f;
  return clampFloat((static_cast<float>(mv) - dryBaselineMv) / denom * 100.0f, 0.0f, 100.0f);
}

float tempFromMv(uint16_t mv) {
  // Outside ~150-3100 mV the pin is at ground or open (no thermistor in circuit). Converting that
  // would print nonsense like 116 C or -30 C, so report "no reading" instead.
  if (mv < 150 || mv > 3100) return NAN;
  float rNtc = K_SERIES * static_cast<float>(mv) / (3300.0f - static_cast<float>(mv));
  float invT = (1.0f / T0K) + (logf(rNtc / R0) / BETA);
  return (1.0f / invT) - 273.15f;
}

float readTempC(int pin) { return tempFromMv(avgMilliVolts(pin, TEMP_SAMPLES)); }

// Feature vector for matching: chromaticity (x100, so 1 point = 1% colour shift) plus log
// brightness. Brightness matters: on this chart pH 9 (dark olive) has nearly the same colour as
// the 7->8 transition and differs mainly in darkness. The sensor sits at a fixed distance in the
// pod, so brightness relative to the white reference is reliable.
static void chartFeature(float cr, float cg, float cb, float lum, float out[4]) {
  out[0] = 100.0f * cr; out[1] = 100.0f * cg; out[2] = 100.0f * cb;
  out[3] = BRIGHTNESS_WEIGHT * logf(fmaxf(lum, 1e-3f));
}

float matchChartPh(float r, float g, float b, const float white[3], float* distOut, float* nearestOut) {
  if (distOut) *distOut = NAN;
  if (nearestOut) *nearestOut = NAN;
  if (!(white[0] > 0 && white[1] > 0 && white[2] > 0)) return NAN;
  const float wr = r / white[0], wg = g / white[1], wb = b / white[2];
  const float sum = wr + wg + wb;
  if (!(sum > 0)) return NAN;
  float f[4];
  chartFeature(wr / sum, wg / sum, wb / sum, sum / 3.0f, f);

  // The chart is a path 1 -> 2 -> ... -> 14 through colour space. Find the closest point on that
  // path: a colour between two squares lands on their segment and reads as an in-between pH, even
  // when some other square happens to sit nearby (pH 9 sits right next to the 7->8 segment).
  const int n = static_cast<int>(sizeof(kChart) / sizeof(kChart[0]));
  float bestD = INFINITY, bestPh = NAN;
  for (int i = 0; i + 1 < n; i++) {
    float A[4], B[4];
    chartFeature(kChart[i].r, kChart[i].g, kChart[i].b, kChart[i].lum, A);
    chartFeature(kChart[i + 1].r, kChart[i + 1].g, kChart[i + 1].b, kChart[i + 1].lum, B);
    float ab2 = 0, apab = 0;
    for (int k = 0; k < 4; k++) {
      ab2 += (B[k] - A[k]) * (B[k] - A[k]);
      apab += (f[k] - A[k]) * (B[k] - A[k]);
    }
    const float t = ab2 > 0 ? fminf(1.0f, fmaxf(0.0f, apab / ab2)) : 0.0f;
    float d2 = 0;
    for (int k = 0; k < 4; k++) {
      const float q = A[k] + t * (B[k] - A[k]) - f[k];
      d2 += q * q;
    }
    const float d = sqrtf(d2);
    if (d < bestD) {
      bestD = d;
      bestPh = kChart[i].pH + t * (kChart[i + 1].pH - kChart[i].pH);
    }
  }
  if (distOut) *distOut = bestD;
  if (nearestOut) *nearestOut = bestPh;
  return bestD <= g_phRadius ? bestPh : NAN;  // too far off the chart: not a strip colour
}

float readPH() {
  const bool calibrated = g_white[0] > 0;
  g_color = ColorReading{0, 0, 0, 0, NAN, NAN, NAN, calibrated};
  uint16_t r, g, b, c;
  if (!readColorRaw(r, g, b, c)) return NAN;
  g_color.r = r; g_color.g = g; g_color.b = b; g_color.c = c;
  if (c < CLEAR_TOO_DARK || c >= CLEAR_SATURATED) return NAN;  // nothing there, or glare
  float d, nearest;
  const float matched = matchChartPh(r, g, b, calibrated ? g_white : kDefaultWhite, &d, &nearest);
  g_color.matchDist = d;
  g_color.nearestPh = nearest;
  // Only a calibrated, on-chart match is trusted (and can drive alerts).
  g_color.pH = calibrated ? matched : NAN;
  return g_color.pH;
}

const ColorReading& lastColor() { return g_color; }

void setPhMatchRadius(float points) { g_phRadius = points; }

void setWhiteRef(const uint16_t rgb[3]) {
  for (int i = 0; i < 3; i++) g_white[i] = rgb[i];
}

bool captureWhiteRef(uint16_t out[3]) {
  uint16_t r, g, b, c;
  if (!readColorRaw(r, g, b, c)) return false;
  if (c < 500 || c >= CLEAR_SATURATED || r == 0 || g == 0 || b == 0) return false;
  out[0] = r; out[1] = g; out[2] = b;
  setWhiteRef(out);
  return true;
}

// Flex is reported FLIPPED (3300 - pin mV): the pin reads ~3100 mV flat and drops as it bends, so
// flipped it reads ~200 flat and rises with bend. 16 samples: steadier at the higher sensitivity.
uint16_t readLiftMv() {
  const uint32_t pin = avgMilliVolts(Pins::LIFT, 16);
  return pin >= 3300 ? 0 : static_cast<uint16_t>(3300 - pin);
}

float liftNormalized(uint16_t mv, const SessionState& session, float fullScaleMv) {
  // Flat is captured at New dressing; the span comes from the live sensitivity setting, so
  // moving the slider takes effect immediately without a new dressing. liftMv is the flipped
  // reading, so it rises as the flex bends.
  float denom = fullScaleMv;
  if (!session.active || fabsf(denom) < 1.0f) return 0.0f;
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
  session.baselineDryEdgeMv = Pins::EDGE_ZONE_FITTED ? readMoistureMv(Pins::M_EDGE) : 0;
  session.baselineColorPh = readPH();
  session.liftFlatMv = readLiftMv();
  // Lift sensitivity: lift reaches 1.0 after this many mV above the flat (flipped) reading.
  // 300 mV makes a modest bend register clearly (lift 0.25 alert = +75 mV).
  session.liftMaxBendMv = session.liftFlatMv + LIFT_FULL_SCALE_MV;
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
  // Measured on the Feather V2: the ADC floors at ~142 mV (0 V) and tops out at ~3150 mV (3.3 V).
  // Limits must sit inside that range or they can never fire.
  constexpr uint16_t AT_GROUND_MV = 150, AT_RAIL_MV = 3100;
  // tempFromMv() returns NAN for an open or shorted thermistor, so NAN is a fault too.
  auto tempBad = [](float c) { return isnan(c) || c < 15.0f || c > 50.0f; };
#ifndef SKIP_MOISTURE
  // A dry pad legitimately reads ~0 V, so only a node stuck at 3.3 V (shorted to EXC) is a fault.
  if (r.centerMv >= AT_RAIL_MV || (Pins::EDGE_ZONE_FITTED && r.edgeMv >= AT_RAIL_MV)) f |= FAULT_MOISTURE_OPEN;
#endif
  if (tempBad(r.tWoundC) || tempBad(r.tRefC)) f |= FAULT_TEMP_RANGE;
  if (!r.colorPresent) f |= FAULT_COLOR_MISSING;
  if (r.colorPresent && r.clearRaw >= CLEAR_SATURATED) f |= FAULT_AMBIENT_LIGHT;
  // liftMv is flipped (3300 - pin mV). Flat reads ~200; only a pin stuck at 0 V (no 3V feed /
  // shorted), i.e. a flipped reading of 3150+, is a fault. An unplugged flex can also look flat,
  // so the dashboard says to bend it to confirm.
  if (r.liftMv >= 3300 - AT_GROUND_MV) f |= FAULT_FLEX_OPEN;
  return f;
}

uint16_t sensorsLastClear() { return g_lastClear; }
