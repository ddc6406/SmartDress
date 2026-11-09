#include <Arduino.h>
#include <Preferences.h>
#include <atomic>

#include "pins.h"
#include "sensors.h"
#include "decision.h"
#include "alerts.h"
#include "ble_service.h"

#ifdef BENCH_CMDS
#include "driver/rtc_io.h"
#endif

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
  // Buzz on/off get their own flags: a quick tap sends ON then OFF back to back, and a single
  // command slot would let OFF overwrite ON before loop() sees it (no buzz at all).
  std::atomic<bool> buzzOnPending{false};
  std::atomic<bool> buzzOffPending{false};
  uint32_t buzzStartMs = 0;
  constexpr uint32_t BUZZ_MAX_MS = 5000;  // safety: never run longer than this per press
  constexpr uint32_t BUZZ_MIN_MS = 250;   // a quick tap still gives a buzz you can feel
  std::atomic<bool> thresholdsPending{false};
  Thresholds pendingThresholds{};

  uint32_t lastMoistureMs = 0;
  uint32_t lastTempMs = 0;
  uint32_t lastLiftMs = 0;
  uint32_t lastPhMs = 0;
  uint32_t lastBleMs = 0;
  uint32_t lastLogMs = 0;
  uint32_t lastSaveMs = 0;

#ifdef BENCH_CMDS
  constexpr uint32_t INTERVAL_MOISTURE_MS = 2000;   // bench: near-live moisture on the dashboard
#else
  constexpr uint32_t INTERVAL_MOISTURE_MS = 10000;  // pulse rarely to limit thread corrosion
#endif
  constexpr uint32_t INTERVAL_TEMP_MS = 1000;
  constexpr uint32_t INTERVAL_LIFT_MS = 1000;
#ifdef BENCH_CMDS
  constexpr uint32_t INTERVAL_PH_MS = 2000;   // bench: near-live colour readings
#else
  constexpr uint32_t INTERVAL_PH_MS = 60000;
#endif
  constexpr uint32_t INTERVAL_BLE_MS = 1000;
  constexpr uint32_t INTERVAL_LOG_MS = 5000;
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

  struct WhiteRef { uint16_t rgb[3]; };

  void loadWhiteRef() {
    WhiteRef w{};
    if (loadBlob("white", w)) setWhiteRef(w.rgb);
  }

  void calibrateWhite() {
    WhiteRef w{};
    if (captureWhiteRef(w.rgb)) {
      saveBlob("white", w);
      Serial.printf("[colour] white reference saved: R=%u G=%u B=%u\n", w.rgb[0], w.rgb[1], w.rgb[2]);
      lastPhMs = millis() - INTERVAL_PH_MS;  // re-read the strip straight away with the new reference
    } else {
      Serial.println("[colour] white reference FAILED: no sensor, too dark, or glare. Hold it 3-5 mm over white paper.");
    }
  }

  void loadThresholds() {
    Thresholds t{};
    if (loadBlob("thresh", t) && thresholdsValid(t)) thresholds = t;
    setPhMatchRadius(thresholds.phMatchRadius);
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
#ifdef SKIP_MOISTURE
      // Unwired inputs float and give random readings; hold moisture at 0 instead.
      latest.centerPct = latest.edgePct = filtCenter = filtEdge = 0.0f;
#else
      latest.centerMv = readMoistureMv(Pins::M_CENTER);
      filtCenter = ema(filtCenter, moisturePercent(latest.centerMv, session.baselineDryCenterMv, thresholds.moistureWetMv));
      latest.centerPct = filtCenter;
      if (Pins::EDGE_ZONE_FITTED) {
        latest.edgeMv = readMoistureMv(Pins::M_EDGE);
        filtEdge = ema(filtEdge, moisturePercent(latest.edgeMv, session.baselineDryEdgeMv, thresholds.moistureWetMv));
        latest.edgePct = filtEdge;
      } else {
        latest.edgeMv = 0;       // no edge thread: nothing to read
        latest.edgePct = NAN;    // "unavailable" on the wire; the edge-wet rule can never fire
      }
#endif
    }

    if (due(lastTempMs, INTERVAL_TEMP_MS)) {
      latest.tWoundMv = readPinMv(Pins::T_WOUND);
      latest.tRefMv = readPinMv(Pins::T_REF);
      latest.tWoundC = tempFromMv(latest.tWoundMv);
      latest.tRefC = tempFromMv(latest.tRefMv);
      // An open/shorted thermistor reads NAN; checkFaults() reports it, so don't fake a 0 here.
      if (!isnan(latest.tWoundC) && !isnan(latest.tRefC)) {
        filtTempDiff = ema(filtTempDiff, (latest.tWoundC - latest.tRefC) - session.thermistorOffsetC);
      }
      latest.tempDiffC = filtTempDiff;  // NAN until both thermistors give real readings
      if (!isnan(filtTempDiff) && maybeLockTempBaseline(session, latest.tempDiffC)) {
        saveSession();
      }
    }

    if (due(lastLiftMs, INTERVAL_LIFT_MS)) {
      latest.liftMv = readLiftMv();
      latest.lift = liftNormalized(latest.liftMv, session, thresholds.liftFullScaleMv);
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
    uint16_t e = Pins::EDGE_ZONE_FITTED ? readMoistureMv(Pins::M_EDGE) : 0;
    float tw = readTempC(Pins::T_WOUND);
    float tr = readTempC(Pins::T_REF);
    uint16_t lift = readLiftMv();
    float ph = readPH();
    if (Pins::EDGE_ZONE_FITTED) Serial.printf("  moisture mV C/E=%u/%u\n", c, e);
    else Serial.printf("  moisture mV=%u (single zone on A2)\n", c);
    Serial.printf("  temp C wound/ref=%.2f/%.2f\n", tw, tr);
    const ColorReading& col = lastColor();
    Serial.printf("  lift mV=%u  pH trusted=%.2f best-guess=%.2f  color=%s  RGBC=%u/%u/%u/%u off-chart=%.1f white=%s\n", lift, ph,
                  col.nearestPh, sensorsColorPresent() ? "yes" : "no", col.r, col.g, col.b, col.c, col.matchDist,
                  col.whiteSet ? "set" : "NOT SET (best guess uses default white)");
    Serial.println("[self-test] ----");
  }

#ifdef BENCH_CMDS
  // Plain-English verdict per analog sensor from its raw pin voltage. The ESP32 ADC reads
  // about 140 mV even at 0 V, so anything at or below ~150 mV means "at ground".
  const char* pinVerdict(uint16_t mv, bool pullUpToVcc) {
    if (mv <= 150) {
      return pullUpToVcc ? "AT GROUND: pull-up resistor to 3V not connected, or pin/sensor shorted to GND"
                         : "at ground (dry/open)";
    }
    if (mv >= 3100) {
      return pullUpToVcc ? "AT 3V: sensor not connected (open) or its GND leg is missing" : "at 3V (shorted)";
    }
    return "in range";
  }

  void runDiagnose() {
    uint16_t w = readPinMv(Pins::T_WOUND), r = readPinMv(Pins::T_REF), f = readPinMv(Pins::LIFT);
    Serial.println("[diagnose] ----");
    Serial.printf("  wound thermistor GPIO%d: %umV  %s\n", Pins::T_WOUND, w, pinVerdict(w, true));
    Serial.printf("  ref   thermistor GPIO%d: %umV  %s\n", Pins::T_REF, r, pinVerdict(r, true));
    Serial.printf("  flex             GPIO%d: %umV  %s\n", Pins::LIFT, f,
                  f <= 150 ? "at ground: no 3V feed or shorted" : "OK (~3100 flat; bend it and it should drop)");
    Serial.println("[diagnose] ----");
  }
#endif

#ifdef BENCH_CMDS
  // Wiring probe: pin "32" (A7) is unused by DressCheck. Its internal ~45k pull-up/pull-down
  // pushes at most ~70 uA into whatever breadboard row a jumper connects it to, so it is safe
  // to touch any row, including 3V and GND. Comparing the reading under each pull tells us
  // what that row is, and which sensor pins move with it tells us which wire it belongs to.
  constexpr gpio_num_t PROBE_PIN = GPIO_NUM_32;
  bool probeMode = false;
  uint32_t lastProbeMs = 0;

  struct WatchedPin { int gpio; const char* label; };
  const WatchedPin kWatched[] = {
      {Pins::M_CENTER, "A2 (centre moisture)"}, {Pins::M_EDGE, "A3 (edge moisture)"},
      {Pins::T_WOUND, "A4 (wound thermistor)"}, {Pins::T_REF, "37 (reference thermistor)"},
      {Pins::LIFT, "33 (flex)"},
  };
  constexpr int N_WATCHED = sizeof(kWatched) / sizeof(kWatched[0]);

  void setProbePull(bool up) {
    rtc_gpio_pullup_dis(PROBE_PIN);
    rtc_gpio_pulldown_dis(PROBE_PIN);
    if (up) rtc_gpio_pullup_en(PROBE_PIN);
    else rtc_gpio_pulldown_en(PROBE_PIN);
    delay(10);
  }

  void runProbe() {
    readPinMv(PROBE_PIN);  // first read configures the pad as analog (and clears pulls); do it before setting them
    uint16_t up, dn, wUp[N_WATCHED], wDn[N_WATCHED];
    setProbePull(true);
    up = readPinMv(PROBE_PIN);
    for (int i = 0; i < N_WATCHED; i++) wUp[i] = readPinMv(kWatched[i].gpio);
    setProbePull(false);
    dn = readPinMv(PROBE_PIN);
    for (int i = 0; i < N_WATCHED; i++) wDn[i] = readPinMv(kWatched[i].gpio);
    rtc_gpio_pullup_dis(PROBE_PIN);
    rtc_gpio_pulldown_dis(PROBE_PIN);

    const char* what;
    if (up >= 2900 && dn >= 2900) what = "TIED TO 3V (rail is live)";
    else if (up <= 200 && dn <= 200) what = "TIED TO GND";
    else if (up - static_cast<int>(dn) > 2500) what = "NOTHING CONNECTED (open)";
    else what = "a resistor/sensor node (something is connected)";
    Serial.printf("[probe] row = %u mV pulled up / %u mV pulled down -> %s\n", up, dn, what);
    bool any = false;
    for (int i = 0; i < N_WATCHED; i++) {
      int moved = static_cast<int>(wUp[i]) - static_cast<int>(wDn[i]);
      // A real connection follows the pull (moves up when pulled up). Ignore opposite-direction
      // shifts: those are pad-to-pad coupling, not continuity.
      if (moved > 60) {
        Serial.printf("        connected to Feather pin %s (moved %d mV)\n", kWatched[i].label, moved);
        any = true;
      }
    }
    if (!any) Serial.println("        not connected to any DressCheck sensor pin");
  }
#endif

  void onThresholdsWrite(const Thresholds& t) {
    pendingThresholds = t;
    thresholdsPending.store(true);
  }

  void onCommand(BleCommand cmd) {
    if (cmd == CMD_BUZZ_ON) buzzOnPending.store(true);
    else if (cmd == CMD_BUZZ_OFF) buzzOffPending.store(true);
    else pendingCommand.store(cmd);
  }

  void applyPendingBle() {
    if (buzzOnPending.exchange(false)) {
      buzzStartMs = millis();
      pulseMotor(BUZZ_MAX_MS);
    }
    if (buzzOffPending.exchange(false)) {
      const uint32_t held = millis() - buzzStartMs;
      if (held < BUZZ_MIN_MS) pulseMotor(BUZZ_MIN_MS - held);  // finish the minimum buzz
      else stopVibration();
    }
    if (thresholdsPending.exchange(false)) {
      thresholds = pendingThresholds;
      setPhMatchRadius(thresholds.phMatchRadius);
      saveThresholds();
      Serial.printf("[settings] flex full-scale=%.0f mV  wet=%.0f mV  pH leniency=%.1f pts  lift alert=%.2f\n",
                    thresholds.liftFullScaleMv, thresholds.moistureWetMv, thresholds.phMatchRadius,
                    thresholds.liftThreshold);
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
      case CMD_CALIBRATE_WHITE:
        calibrateWhite();
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
#ifdef BENCH_CMDS
  Serial.println("[BENCH] serial commands: n=new dressing w=white ref s=self-test d=diagnose r=raw p=wiring probe a=ack v=buzz motor");
#endif
#ifdef SKIP_MOISTURE
  Serial.println("[BENCH] moisture sensing SKIPPED (-DSKIP_MOISTURE). Not for the demo.");
#endif
  loadThresholds();
  loadSession();
  loadWhiteRef();

  bleInit(onThresholdsWrite, onCommand);
  bleSetThresholdsValue(thresholds);

  sampleSensors(millis(), true);  // real readings before the first packet, session or not
  if (session.active) {
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
#ifdef BENCH_CMDS
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'n': startNewDressing(); break;
      case 'w': calibrateWhite(); break;
      case 's': runSelfTest(); break;
      case 'a': alertAcknowledged = true; stopVibration(); break;
      case 'r':
        Serial.printf("[raw] wound=%4umV ref=%4umV flex=%4umV | Tw=%.1fC Tr=%.1fC\n",
                      readPinMv(Pins::T_WOUND), readPinMv(Pins::T_REF), readPinMv(Pins::LIFT),
                      readTempC(Pins::T_WOUND), readTempC(Pins::T_REF));
        break;
      case 'd': runDiagnose(); break;
      case 'p':
        probeMode = !probeMode;
        Serial.printf("[probe] %s (jumper from pin 32's row to the row you want to test)\n", probeMode ? "ON" : "OFF");
        break;
      case 'v': Serial.println("[bench] motor on 600 ms"); pulseMotor(600); break;
      default: break;
    }
  }
#endif

  if (buttonPressed()) {
    alertAcknowledged = true;
    stopVibration();
  }
  if (buttonLongPressed()) {
    startNewDressing();
  }

#ifdef BENCH_CMDS
  if (probeMode && millis() - lastProbeMs >= 1000) {
    lastProbeMs = millis();
    runProbe();
  }
#endif

  // Taken after the handlers above: calibration blocks for ~2 s, and a stale `now`
  // would make now - session.startMs underflow.
  const uint32_t now = millis();

  // Raw sensor data streams whether or not a dressing session is active, so the dashboard is
  // live as soon as the pod is powered. Decisions and alerts need a session's calibration.
  sampleSensors(now, false);
  latest.faults = checkFaults(latest);
  DecisionResult decision{};
  decision.urgency = Urgency::OK;

  if (session.active) {
    if (now - lastSaveMs >= INTERVAL_SAVE_MS) {
      lastSaveMs = now;
      saveSession();
    }

    latest.hoursWorn = (now - session.startMs) / 3600000.0f;
    decision = evaluateStatus(latest, thresholds, session, timers);

    if (decision.urgency != currentUrgency) {
      currentUrgency = decision.urgency;
      alertAcknowledged = false;
      triggerAlert(currentUrgency);
      bleNotifyStatus(currentUrgency, decision.reasons, decision.reasonCount);
      Serial.printf("[main] urgency -> %s (%s)\n", urgencyName(currentUrgency),
                    decision.reasonCount ? reasonPlainLanguage(decision.reasons[0]) : "");
    }

    alertsTick(currentUrgency, alertAcknowledged);
  } else {
    // No dry/flat baselines yet, so %, lift and wear time would be made-up numbers: send "unavailable".
    latest.centerPct = latest.edgePct = latest.lift = latest.hoursWorn = NAN;
    alertsTick(Urgency::OK, true);  // still runs the motor-off timer (e.g. after a bench buzz test)
    // Idle pulse while waiting for a session
    bool on = ((now / 800) % 2) == 0;
    setNeoPixelRGB(on ? 40 : 10, on ? 80 : 20, on ? 220 : 60);
  }

  if (now - lastBleMs >= INTERVAL_BLE_MS) {
    lastBleMs = now;
    SensorPacket pkt = makePacket(latest, decision);
    bleNotifySensorPacket(pkt);
  }

  if (now - lastLogMs >= INTERVAL_LOG_MS) {
    lastLogMs = now;
    Serial.printf("[pkt] mV C=%u E=%u flex=%u Tw=%u Tr=%u | Tw=%.1fC Tr=%.1fC dT=%.2f lift=%.2f h=%.1f faults=0x%02x %s\n",
                  latest.centerMv, latest.edgeMv, latest.liftMv, latest.tWoundMv, latest.tRefMv,
                  latest.tWoundC, latest.tRefC, latest.tempDiffC, latest.lift, latest.hoursWorn,
                  latest.faults, session.active ? urgencyName(currentUrgency) : "NO_SESSION");
  }

  delay(20);
}
