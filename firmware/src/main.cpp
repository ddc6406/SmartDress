#include <Arduino.h>
#include <Preferences.h>
#include <atomic>

#include "pins.h"
#include "sensors.h"
#include "decision.h"
#include "alerts.h"
#include "ble_service.h"

namespace {
  constexpr const char* NVS_NAMESPACE = "dresscheck";

  Preferences prefs;
  Thresholds thresholds = defaultThresholds();
  SessionState session{};
  DecisionTimers timers{};
  Urgency currentUrgency = Urgency::OK;
  bool alertAcknowledged = false;

  SensorReadings latest{};
  // NAN = not seeded yet; the first sample initialises the filter instead of easing in from 0.
  float filtCenter = NAN, filtEdge = NAN, filtTempDiff = NAN, filtPh = NAN;

  // BLE callbacks run on the BLE host task. They only queue work here; loop() applies it,
  // so session/threshold state is never touched from two tasks at once.
  std::atomic<uint8_t> pendingCommand{CMD_NONE};
  std::atomic<bool> thresholdsPending{false};
  Thresholds pendingThresholds{};

  uint32_t lastMoistureMs = 0;
  uint32_t lastTempMs = 0;
  uint32_t lastLiftMs = 0;
  uint32_t lastPhMs = 0;
  uint32_t lastBleMs = 0;
  uint32_t lastSaveMs = 0;

  constexpr uint32_t INTERVAL_MOISTURE_MS = 10000;
  constexpr uint32_t INTERVAL_TEMP_MS = 5000;
  constexpr uint32_t INTERVAL_LIFT_MS = 1000;
  constexpr uint32_t INTERVAL_PH_MS = 60000;
  constexpr uint32_t INTERVAL_BLE_MS = 5000;
  constexpr uint32_t INTERVAL_SAVE_MS = 5UL * 60UL * 1000UL;  // wear-time checkpoint

  float ema(float prev, float sample, float alpha = 0.35f) {
    if (isnan(sample)) return prev;
    if (isnan(prev)) return sample;
    return prev + alpha * (sample - prev);
  }

  template <typename T>
  void saveBlob(const char* key, const T& value) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBytes(key, &value, sizeof(T));
    prefs.end();
  }

  // Only accepts a blob written by this exact struct layout, so a firmware update that
  // changes a struct falls back to defaults instead of loading garbage.
  template <typename T>
  bool loadBlob(const char* key, T& out) {
    T tmp{};
    prefs.begin(NVS_NAMESPACE, true);
    bool ok = prefs.getBytesLength(key) == sizeof(T) &&
              prefs.getBytes(key, &tmp, sizeof(T)) == sizeof(T);
    prefs.end();
    if (ok) out = tmp;
    return ok;
  }

  void saveThresholds() { saveBlob("thresh", thresholds); }

  void loadThresholds() {
    Thresholds t{};
    if (loadBlob("thresh", t) && thresholdsValid(t)) thresholds = t;
  }

  void saveSession() {
    if (session.active) session.wornMs = millis() - session.startMs;
    saveBlob("session", session);
  }

  void loadSession() {
    if (!loadBlob("session", session)) return;
    // millis() restarts at 0 on boot, so the saved startMs is meaningless; rebuild it.
    if (session.active) session.startMs = millis() - session.wornMs;
  }

  // Read every sensor that is due (or all of them when force is set).
  void sampleSensors(uint32_t now, bool force) {
    auto due = [&](uint32_t& last, uint32_t interval) {
      if (!force && now - last < interval) return false;
      last = now;
      return true;
    };

    if (due(lastMoistureMs, INTERVAL_MOISTURE_MS)) {
      latest.centerMv = readMoistureMv(Pins::M_CENTER);
      latest.edgeMv = readMoistureMv(Pins::M_EDGE);
      filtCenter = ema(filtCenter, moisturePercent(latest.centerMv, session.baselineDryCenterMv));
      filtEdge = ema(filtEdge, moisturePercent(latest.edgeMv, session.baselineDryEdgeMv));
      latest.centerPct = filtCenter;
      latest.edgePct = filtEdge;
    }

    if (due(lastTempMs, INTERVAL_TEMP_MS)) {
      latest.tWoundC = readTempC(Pins::T_WOUND);
      latest.tRefC = readTempC(Pins::T_REF);
      // An open/shorted thermistor reads NAN; checkFaults() reports it, so don't fake a 0 here.
      if (!isnan(latest.tWoundC) && !isnan(latest.tRefC)) {
        filtTempDiff = ema(filtTempDiff, (latest.tWoundC - latest.tRefC) - session.thermistorOffsetC);
      }
      latest.tempDiffC = isnan(filtTempDiff) ? 0.0f : filtTempDiff;
      if (!isnan(filtTempDiff) && maybeLockTempBaseline(session, latest.tempDiffC)) {
        saveSession();
      }
    }

    if (due(lastLiftMs, INTERVAL_LIFT_MS)) {
      latest.liftMv = readLiftMv();
      latest.lift = liftNormalized(latest.liftMv, session);
    }

    if (due(lastPhMs, INTERVAL_PH_MS)) {
      float ph = readPH();
      filtPh = ema(filtPh, ph);
      latest.pHEst = isnan(ph) ? NAN : filtPh;
      latest.clearRaw = sensorsLastClear();
      latest.colorPresent = sensorsColorPresent();
    }
  }

  void startNewDressing() {
    newDressingCalibration(session);
    timers = {};
    alertAcknowledged = false;
    currentUrgency = Urgency::OK;
    // Drop everything measured on the previous dressing, otherwise its wet readings
    // would re-trigger an alert straight after the change.
    latest = {};
    filtCenter = filtEdge = filtTempDiff = filtPh = NAN;
    sampleSensors(millis(), true);
    saveSession();
    setNeoPixelRGB(40, 80, 220);  // blue — calibrating
    Serial.println("[main] new dressing session started");
  }

  void runSelfTest() {
    Serial.println("[self-test] ----");
    uint16_t c = readMoistureMv(Pins::M_CENTER);
    uint16_t e = readMoistureMv(Pins::M_EDGE);
    float tw = readTempC(Pins::T_WOUND);
    float tr = readTempC(Pins::T_REF);
    uint16_t lift = readLiftMv();
    float ph = readPH();
    Serial.printf("  moisture mV C/E=%u/%u\n", c, e);
    Serial.printf("  temp C wound/ref=%.2f/%.2f\n", tw, tr);
    Serial.printf("  lift mV=%u  pH_est=%.2f  color=%s\n", lift, ph,
                  sensorsColorPresent() ? "yes" : "no");
    Serial.println("[self-test] ----");
  }

  void onThresholdsWrite(const Thresholds& t) {
    pendingThresholds = t;
    thresholdsPending.store(true);
  }

  void onCommand(BleCommand cmd) { pendingCommand.store(cmd); }

  void applyPendingBle() {
    if (thresholdsPending.exchange(false)) {
      thresholds = pendingThresholds;
      saveThresholds();
    }
    switch (static_cast<BleCommand>(pendingCommand.exchange(CMD_NONE))) {
      case CMD_NEW_DRESSING:
        startNewDressing();
        break;
      case CMD_ACK_ALERT:
        alertAcknowledged = true;
        stopVibration();
        break;
      case CMD_SELF_TEST:
        runSelfTest();
        break;
      default:
        break;
    }
  }
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== DressCheck pod ===");

  pinsInit();
  sensorsInit();
  alertsInit();
  loadThresholds();
  loadSession();

  bleInit(onThresholdsWrite, onCommand);
  bleSetThresholdsValue(thresholds);

  if (session.active) {
    sampleSensors(millis(), true);  // real readings before the first evaluation
    setNeoPixelRGB(0, 180, 40);
    Serial.printf("[main] session restored from flash (%.1f h worn)\n",
                  session.wornMs / 3600000.0f);
  } else {
    setNeoPixelRGB(40, 80, 220);  // waiting for NEW_DRESSING
    Serial.println("[main] no active session — long-press button or send NEW_DRESSING");
  }
}

void loop() {
  applyPendingBle();

  if (buttonPressed()) {
    alertAcknowledged = true;
    stopVibration();
  }
  if (buttonLongPressed()) {
    startNewDressing();
  }

  // Taken after the handlers above: calibration blocks for ~2 s, and a stale `now`
  // would make now - session.startMs underflow.
  const uint32_t now = millis();

  if (session.active) {
    sampleSensors(now, false);

    if (now - lastSaveMs >= INTERVAL_SAVE_MS) {
      lastSaveMs = now;
      saveSession();
    }

    latest.hoursWorn = (now - session.startMs) / 3600000.0f;
    latest.faults = checkFaults(latest);

    DecisionResult decision = evaluateStatus(latest, thresholds, session, timers);

    if (decision.urgency != currentUrgency) {
      currentUrgency = decision.urgency;
      alertAcknowledged = false;
      triggerAlert(currentUrgency);
      bleNotifyStatus(currentUrgency, decision.reasons, decision.reasonCount);
      Serial.printf("[main] urgency -> %s (%s)\n", urgencyName(currentUrgency),
                    decision.reasonCount ? reasonPlainLanguage(decision.reasons[0]) : "");
    }

    alertsTick(currentUrgency, alertAcknowledged);

    if (now - lastBleMs >= INTERVAL_BLE_MS) {
      lastBleMs = now;
      SensorPacket pkt = makePacket(latest, decision);
      bleNotifySensorPacket(pkt);
      Serial.printf("[pkt] C=%.1f E=%.1f dT=%.2f pH=%.2f lift=%.2f h=%.1f faults=0x%02x urg=%s\n",
                    latest.centerPct, latest.edgePct, latest.tempDiffC,
                    isnan(latest.pHEst) ? -1.0f : latest.pHEst, latest.lift,
                    latest.hoursWorn, latest.faults, urgencyName(currentUrgency));
    }
  } else {
    // Idle pulse while waiting for a session
    bool on = ((now / 800) % 2) == 0;
    setNeoPixelRGB(on ? 40 : 10, on ? 80 : 20, on ? 220 : 60);
  }

  delay(20);
}
