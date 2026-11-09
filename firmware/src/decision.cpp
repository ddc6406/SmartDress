#include "decision.h"

Thresholds defaultThresholds() {
  Thresholds t{};
  t.saturationSoonPct = 60.0f;
  t.saturationNowPct = 85.0f;
  t.edgeWetPct = 30.0f;
  t.tempDiffRiseC = 2.0f;
  t.tempSustainMin = 30;
  t.pHAlert = 7.5f;
  t.pHSustainMin = 30;
  t.maxWearHours = 72;
  t.liftThreshold = 1.0f;       // alert once the flex reaches the peeling level
  t.liftFullScaleMv = 650.0f;   // peeling level: flipped flex mV (measured ~600-700 when peeling)
  t.moistureWetMv = 2800.0f;
  t.phMatchRadius = PH_MATCH_RADIUS;
  return t;
}

bool thresholdsValid(const Thresholds& t) {
  // Written as positive range checks so NaN fails every comparison.
  auto in = [](float v, float lo, float hi) { return v >= lo && v <= hi; };
  return in(t.saturationSoonPct, 0, 100) && in(t.saturationNowPct, 0, 100) &&
         t.saturationSoonPct < t.saturationNowPct &&
         in(t.edgeWetPct, 0, 100) && in(t.tempDiffRiseC, 0, 10) &&
         in(t.tempSustainMin, 1, 240) && in(t.pHAlert, 4, 9) &&
         in(t.pHSustainMin, 1, 240) && in(t.maxWearHours, 1, 168) &&
         in(t.liftThreshold, 0, 1) && in(t.liftFullScaleMv, 100, 3000) &&
         in(t.moistureWetMv, 500, 3100) && in(t.phMatchRadius, 2, 20);
}

const char* urgencyName(Urgency u) {
  switch (u) {
    case Urgency::OK: return "OK";
    case Urgency::CHANGE_SOON: return "CHANGE_SOON";
    case Urgency::CHANGE_NOW: return "CHANGE_NOW";
    case Urgency::CONTACT_CLINIC: return "CONTACT_CLINIC";
    case Urgency::SENSOR_FAULT: return "SENSOR_FAULT";
  }
  return "UNKNOWN";
}

const char* reasonPlainLanguage(uint8_t code) {
  switch (code) {
    case REASON_TEMP_SUSTAINED:
      return "Wound area warmer than surrounding skin for your clinician's sustain window — contact your clinic for review.";
    case REASON_PH_SUSTAINED:
      return "Estimated fluid pH is above your clinician's limit — contact your clinic for review.";
    case REASON_EDGE_WET:
      return "Fluid is reaching the dressing edge — your clinician's criteria say a leak may be likely.";
    case REASON_SATURATED:
      return "Your clinician's criteria say the dressing is saturated — time to change your dressing.";
    case REASON_LIFT:
      return "Dressing lift / seal change detected against your clinician's threshold.";
    case REASON_MAX_WEAR:
      return "Maximum wear time set by your clinician has been reached.";
    case REASON_SATURATION_SOON:
      return "Dressing moisture is approaching your clinician's change threshold.";
    case REASON_SENSOR_FAULT:
      return "A sensor reading looks open, shorted, or out of range — check snaps and seating.";
    default:
      return "";
  }
}

static void addReason(DecisionResult& out, uint8_t code) {
  if (out.reasonCount >= 4) return;
  out.reasons[out.reasonCount++] = code;
}

DecisionResult evaluateStatus(const SensorReadings& r, const Thresholds& t,
                              const SessionState& session, DecisionTimers& timers) {
  DecisionResult out{};
  out.urgency = Urgency::OK;
  out.reasonCount = 0;
  for (int i = 0; i < 4; i++) out.reasons[i] = REASON_NONE;

  if (r.faults & FAULTS_BLOCKING) {
    out.urgency = Urgency::SENSOR_FAULT;
    addReason(out, REASON_SENSOR_FAULT);
    return out;
  }

  const uint32_t now = millis();

  // Infection-related: sustained only
  if (session.baselineReady) {
    if ((r.tempDiffC - session.baselineTempDiffC) > t.tempDiffRiseC) {
      if (timers.tempOverSinceMs == 0) timers.tempOverSinceMs = now;
    } else {
      timers.tempOverSinceMs = 0;
    }
    if (timers.tempOverSinceMs != 0 &&
        (now - timers.tempOverSinceMs) >= static_cast<uint32_t>(t.tempSustainMin) * 60000UL) {
      addReason(out, REASON_TEMP_SUSTAINED);
    }
  }

  if (!isnan(r.pHEst) && r.pHEst > t.pHAlert) {
    if (timers.pHOverSinceMs == 0) timers.pHOverSinceMs = now;
  } else {
    timers.pHOverSinceMs = 0;
  }
  if (timers.pHOverSinceMs != 0 &&
      (now - timers.pHOverSinceMs) >= static_cast<uint32_t>(t.pHSustainMin) * 60000UL) {
    addReason(out, REASON_PH_SUSTAINED);
  }

  if (out.reasonCount > 0) {
    out.urgency = Urgency::CONTACT_CLINIC;
    return out;
  }

  // Change now
  if (r.edgePct >= t.edgeWetPct) addReason(out, REASON_EDGE_WET);
  if (r.centerPct >= t.saturationNowPct) addReason(out, REASON_SATURATED);
  if (r.lift >= t.liftThreshold) addReason(out, REASON_LIFT);
  // Wear-time alert removed: the team chose not to use a maximum wear time.

  if (out.reasonCount > 0) {
    out.urgency = Urgency::CHANGE_NOW;
    return out;
  }

  // Change soon
  if (r.centerPct >= t.saturationSoonPct) {
    addReason(out, REASON_SATURATION_SOON);
    out.urgency = Urgency::CHANGE_SOON;
    return out;
  }

  out.urgency = Urgency::OK;
  return out;
}
