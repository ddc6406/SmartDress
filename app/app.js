/**
 * DressCheck Web Bluetooth dashboard: live readings, trends, and plain-language advisories.
 * Packet / threshold layouts mirror firmware/include/ble_service.h and ble_service.cpp.
 */

const SERVICE_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000001";
const SENSOR_PACKET_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000002";
const STATUS_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000003";
const THRESHOLDS_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000004";
const COMMAND_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000005";

const CMD = { NEW_DRESSING: 1, ACK_ALERT: 2, SELF_TEST: 3, CALIBRATE_WHITE: 4, BUZZ_ON: 5, BUZZ_OFF: 6 };

const REASON_TEXT = {
  0: "",
  1: "Wound area warmer than surrounding skin for your clinician's sustain window — contact your clinic for review.",
  2: "Estimated fluid pH is above your clinician's limit — contact your clinic for review.",
  3: "Fluid is reaching the dressing edge — your clinician's criteria say a leak may be likely.",
  4: "Your clinician's criteria say the dressing is saturated — time to change your dressing.",
  5: "Dressing lift / seal change detected against your clinician's threshold.",
  6: "Maximum wear time set by your clinician has been reached.",
  7: "Dressing moisture is approaching your clinician's change threshold.",
  8: "A sensor reading looks open, shorted, or out of range — check snaps and seating.",
};

const HISTORY_MAX = 120;

// Raw analog channels; offset = byte position in SENSOR_PACKET (edge moisture at 28 is not fitted). verdict(mv) explains what the voltage means for that circuit.
// Measured on the Feather V2: the ADC reads ~142 mV at 0 V and tops out at ~3150 mV at 3.3 V,
// so <= 150 mV means "at ground / nothing connected" and >= 3100 mV means "at 3.3 V".
// Sensor resistance from the pin voltage, using each circuit's known fixed resistor (3.3 V supply):
//   "top"    = sensor between 3.3 V and the pin, fixed resistor to GND:  R = Rfix * (3300 - V) / V
//   "bottom" = fixed resistor from 3.3 V to the pin, sensor to GND:      R = Rfix * V / (3300 - V)
// The ADC can't resolve below ~150 mV or above ~3100 mV, so beyond those it's shown as a bound.
function sensorResistance(mv, rFixed, sensorOnTop) {
  if (mv == null) return "—";
  const v = Math.min(Math.max(mv, 150), 3100);
  const r = sensorOnTop ? (rFixed * (3300 - v)) / v : (rFixed * v) / (3300 - v);
  const text = r >= 1e6 ? `${(r / 1e6).toFixed(2)} MΩ` : `${(r / 1e3).toFixed(1)} kΩ`;
  if (mv <= 150) return sensorOnTop ? `over ${text} (open / dry)` : `under ${text} (shorted)`;
  if (mv >= 3100) return sensorOnTop ? `under ${text} (shorted)` : `over ${text} (open)`;
  return text;
}

const RAW_CHANNELS = [
  { key: "centerMv", offset: 26, name: "Moisture", simple: "moisture sensor", pin: "A2", color: "--chart-a", rFixed: 100000, sensorOnTop: true, rLabel: "Pad (thread on A0 → thread on A2)",
    range: "142 mV = dry (pad over 2 MΩ) · rises as it gets wet: ~500 damp, 1500–2800 wet · 3100+ = shorted",
    verdict: (mv) => mv <= 150 ? ["0 V: dry pad, or not connected", false] : mv >= 3100 ? ["Stuck at 3.3 V: shorted", false] : ["Wet signal", true] },
  { key: "liftMv", offset: 30, name: "Flex", simple: "flex sensor", pin: "33", color: "--chart-c", rFixed: 47000, sensorOnTop: false, rLabel: "Flex sensor",
    range: "Flat: a few hundred mV (10–25 kΩ) · bending raises it (up to ~100 kΩ) · ≤150 or ≥2800 mV = fault",
    verdict: (mv) => mv <= 150 ? ["No signal: check 47k to 3V / wire", false] : mv >= 2800 ? ["Open: flex not connected (> ~300 kΩ)", false] : ["In range (bend it: mV should rise)", true] },
  { key: "tWoundMv", offset: 32, name: "Wound thermistor", simple: "temperature sensor", pin: "A4", color: "--chart-d", rFixed: 10000, sensorOnTop: false, rLabel: "Thermistor",
    range: "~1650 mV / 10 kΩ = 25 °C · ~1300 mV / 6.5 kΩ = skin (~34 °C) · lower = warmer · ≤150 or ≥3100 = fault",
    verdict: (mv) => mv <= 150 ? ["No signal (0 V): A4 wire or 10k to 3V", false] : mv >= 3100 ? ["Open: thermistor GND leg loose", false] : ["In range (~1650 mV room, lower = warmer)", true] },
  { key: "tRefMv", offset: 34, name: "Reference thermistor", simple: "temperature sensor", pin: "37", color: "--chart-e", dash: true, rFixed: 10000, sensorOnTop: false, rLabel: "Thermistor",
    range: "~1650 mV / 10 kΩ = 25 °C · ~1300 mV / 6.5 kΩ = skin (~34 °C) · lower = warmer · ≤150 or ≥3100 = fault",
    verdict: (mv) => mv <= 150 ? ["No signal (0 V): pin-37 wire or 10k to 3V", false] : mv >= 3100 ? ["Open: thermistor GND leg loose", false] : ["In range (~1650 mV room, lower = warmer)", true] },
];

const els = {
  connect: document.getElementById("btn-connect"),
  mock: document.getElementById("btn-mock"),
  disconnect: document.getElementById("btn-disconnect"),
  conn: document.getElementById("conn-banner"),
  advisory: document.getElementById("advisory"),
  advisoryText: document.getElementById("advisory-text"),
  form: document.getElementById("threshold-form"),
  formStatus: document.getElementById("form-status"),
  newDressing: document.getElementById("btn-new-dressing"),
  ack: document.getElementById("btn-ack"),
  selfTest: document.getElementById("btn-self-test"),
  vCenter: document.getElementById("v-center"),
  vTemp: document.getElementById("v-temp"),
  vPh: document.getElementById("v-ph"),
  vLift: document.getElementById("v-lift"),
  vHours: document.getElementById("v-hours"),
  vTWound: document.getElementById("v-twound"),
  vTRef: document.getElementById("v-tref"),
  vFaults: document.getElementById("v-faults"),
  chartMoisture: document.getElementById("chart-moisture"),
  chartTemps: document.getElementById("chart-temps"),
  chartTempPh: document.getElementById("chart-temp-ph"),
};

const state = {
  mode: "idle", // idle | ble | mock
  device: null,
  server: null,
  chars: {},
  history: [],
  thresholds: {
    saturationSoonPct: 60,
    saturationNowPct: 85,
    edgeWetPct: 30,
    tempDiffRiseC: 2.0,
    tempSustainMin: 30,
    pHAlert: 7.5,
    pHSustainMin: 30,
    maxWearHours: 72,
    liftThreshold: 0.25,
    liftFullScaleMv: 300,
    moistureWetMv: 2800,
    phMatchRadius: 8,
  },
  mockTimer: null,
  mockT: 0,
};

function fmt(n, digits = 1) {
  if (n == null || Number.isNaN(n)) return "—";
  return Number(n).toFixed(digits);
}

const SENSOR_PACKET_LEN = 22; // minimum; 26+ add temperatures, 36+ raw pin mV, 46+ colour sensor
const MATCH_NO_READING = 0xffff; // no colour sensor, nothing under it, or glare
const TEMP_UNAVAILABLE = -32768;
const VALUE_UNAVAILABLE = 0xffff; // %, lift, hours before a New dressing calibrates them
const scaled = (raw, k) => (raw === VALUE_UNAVAILABLE ? null : raw / k);

// Bit order matches FaultFlags in firmware/include/sensors.h.
const FAULT_NAMES = [
  [1 << 0, "Moisture sensor"],
  [1 << 1, "Temperature sensor"],
  [1 << 2, "Colour sensor (optional)"],
  [1 << 3, "Ambient light"],
  [1 << 4, "Flex sensor"],
];
const THRESHOLDS_LEN = 42; // 9 clinician thresholds + 3 sensitivity settings; mirrors ble_service.cpp

function tempFromWire(x100) {
  return x100 === TEMP_UNAVAILABLE ? null : x100 / 100;
}

// view: DataView over the characteristic value.
function parseSensorPacket(view) {
  if (view.byteLength < SENSOR_PACKET_LEN) return null;
  const reasons = [view.getUint8(18), view.getUint8(19), view.getUint8(20), view.getUint8(21)]
    .filter((c) => c !== 0);
  return {
    timestampMs: view.getUint32(0, true),
    centerPct: scaled(view.getUint16(4, true), 10),
    edgePct: scaled(view.getUint16(6, true), 10),
    tempDiffC: tempFromWire(view.getInt16(8, true)),
    pHEst: view.getUint16(10, true) / 100 || null, // firmware sends 0 when pH is unavailable
    lift: scaled(view.getUint16(12, true), 1000),
    hoursWorn: scaled(view.getUint16(14, true), 10),
    faults: view.getUint8(16),
    urgency: view.getUint8(17),
    reasons,
    tWoundC: view.byteLength >= 26 ? tempFromWire(view.getInt16(22, true)) : null,
    tRefC: view.byteLength >= 26 ? tempFromWire(view.getInt16(24, true)) : null,
    ...(view.byteLength >= 36
      ? Object.fromEntries(RAW_CHANNELS.map((c) => [c.key, view.getUint16(c.offset, true)]))
      : {}),
    ...(view.byteLength >= 50
      ? {
          colR: view.getUint16(36, true),
          colG: view.getUint16(38, true),
          colB: view.getUint16(40, true),
          colC: view.getUint16(42, true),
          colMatch: view.getUint16(44, true), // colour points off the chart path * 10
          colNearestPh: scaled(view.getUint16(46, true), 100), // best guess, always
          colWhiteSet: (view.getUint16(48, true) & 1) === 1,
        }
      : {}),
  };
}

function encodeThresholds(t) {
  const buf = new ArrayBuffer(THRESHOLDS_LEN);
  const view = new DataView(buf);
  view.setFloat32(0, t.saturationSoonPct, true);
  view.setFloat32(4, t.saturationNowPct, true);
  view.setFloat32(8, t.edgeWetPct, true);
  view.setFloat32(12, t.tempDiffRiseC, true);
  view.setUint16(16, t.tempSustainMin, true);
  view.setUint16(18, t.pHSustainMin, true);
  view.setFloat32(20, t.pHAlert, true);
  view.setUint16(24, t.maxWearHours, true);
  view.setFloat32(26, t.liftThreshold, true);
  view.setFloat32(30, t.liftFullScaleMv, true);
  view.setFloat32(34, t.moistureWetMv, true);
  view.setFloat32(38, t.phMatchRadius, true);
  return new Uint8Array(buf);
}

function decodeThresholds(view) {
  if (view.byteLength < THRESHOLDS_LEN) return null;
  return {
    saturationSoonPct: view.getFloat32(0, true),
    saturationNowPct: view.getFloat32(4, true),
    edgeWetPct: view.getFloat32(8, true),
    tempDiffRiseC: view.getFloat32(12, true),
    tempSustainMin: view.getUint16(16, true),
    pHSustainMin: view.getUint16(18, true),
    pHAlert: view.getFloat32(20, true),
    maxWearHours: view.getUint16(24, true),
    liftThreshold: view.getFloat32(26, true),
    liftFullScaleMv: view.getFloat32(30, true),
    moistureWetMv: view.getFloat32(34, true),
    phMatchRadius: view.getFloat32(38, true),
  };
}

// "What the numbers mean" under each main tile. Uses the live clinician thresholds, so the
// guide stays right when a clinician changes a limit.
function renderRanges() {
  const t = state.thresholds;
  const set = (id, text) => { const el = document.getElementById(`range-${id}`); if (el) el.textContent = text; };
  set("center", `0 % = as dry as when applied · 100 % = ${t.moistureWetMv} mV · ${t.saturationSoonPct} %+ change soon · ${t.saturationNowPct} %+ change now. Needs New dressing.`);
  set("temp", `About 0 = wound same as nearby skin. A rise of ${t.tempDiffRiseC} °C+ held for ${t.tempSustainMin} min → contact clinic.`);
  set("ph", `Lower = more acidic. Above your clinician's limit (${t.pHAlert}) for ${t.pHSustainMin} min → contact clinic. Estimate only.`);
  set("lift", `0 = sealed flat · 1 = ${t.liftFullScaleMv} mV of bend · ${t.liftThreshold}+ (≈ +${Math.round(t.liftThreshold * t.liftFullScaleMv)} mV) = lifting, change now. Needs New dressing.`);
  set("colour", `pH 1–14 from your strip chart (lower = more acidic). Match: ≤2 pts exact · ≤5 good · ≤${t.phMatchRadius} rough · over ${t.phMatchRadius} = not a strip colour.`);
  set("hours", `Time since New dressing. Change by ${t.maxWearHours} h (clinician's maximum wear).`);
  const temps = "Room ~18–25 °C · skin ~30–35 °C. Below 15 or above 50 °C, or no reading = sensor fault.";
  set("twound", temps);
  set("tref", temps);
}

const sliders = {}; // input name -> its range slider

function applyThresholdsToForm(t) {
  const f = els.form;
  for (const [k, v] of Object.entries(t)) {
    const shown = Number.isInteger(v) ? v : Number(v.toFixed(2)); // float32 noise like 0.2500000037
    if (f.elements[k]) f.elements[k].value = shown;
    if (sliders[k]) sliders[k].value = shown;
  }
}

// Give every number setting a slider beside it, kept in sync both ways.
function addSliders() {
  for (const box of els.form.querySelectorAll('input[type="number"]')) {
    const slider = document.createElement("input");
    slider.type = "range";
    for (const a of ["min", "max", "step"]) slider[a] = box[a];
    slider.value = box.value;
    slider.setAttribute("aria-label", `${box.name} slider`);
    slider.addEventListener("input", () => { box.value = slider.value; markUnsaved(); });
    box.addEventListener("input", () => { slider.value = box.value; markUnsaved(); });
    box.after(slider);
    sliders[box.name] = slider;
  }
}

function markUnsaved() {
  els.formStatus.textContent = "Changed: press Save to pod to apply.";
}

function readThresholdsFromForm() {
  const f = els.form;
  return {
    saturationSoonPct: Number(f.saturationSoonPct.value),
    saturationNowPct: Number(f.saturationNowPct.value),
    edgeWetPct: Number(f.edgeWetPct.value),
    tempDiffRiseC: Number(f.tempDiffRiseC.value),
    tempSustainMin: Number(f.tempSustainMin.value),
    pHAlert: Number(f.pHAlert.value),
    pHSustainMin: Number(f.pHSustainMin.value),
    maxWearHours: Number(f.maxWearHours.value),
    liftThreshold: Number(f.liftThreshold.value),
    liftFullScaleMv: Number(f.liftFullScaleMv.value),
    moistureWetMv: Number(f.moistureWetMv.value),
    phMatchRadius: Number(f.phMatchRadius.value),
  };
}

function updateTiles(packet) {
  els.vCenter.textContent = fmt(packet.centerPct, 1);
  els.vTemp.textContent = fmt(packet.tempDiffC, 2);
  els.vPh.textContent = fmt(packet.pHEst, 2);
  els.vLift.textContent = fmt(packet.lift, 2);
  els.vHours.textContent = fmt(packet.hoursWorn, 1);
  els.vTWound.textContent = fmt(packet.tWoundC, 1);
  els.vTRef.textContent = fmt(packet.tRefC, 1);
  const faulty = FAULT_NAMES.filter(([bit]) => packet.faults & bit).map(([, name]) => name);
  els.vFaults.textContent =
    (faulty.length ? `Fault: ${faulty.join(", ")}` : "All sensors OK") +
    (packet.hoursWorn == null ? " · No dressing session: press New dressing to calibrate %, lift and wear time." : "");
}

function updateAdvisory(packet) {
  const lines = packet.reasons.map((c) => REASON_TEXT[c]).filter(Boolean);
  if (!lines.length) {
    els.advisory.hidden = true;
    els.advisoryText.textContent = "";
    return;
  }
  els.advisory.hidden = false;
  els.advisoryText.textContent = lines.join(" ");
}

function pushHistory(packet) {
  state.history.push({
    t: Date.now(),
    centerPct: packet.centerPct,
    edgePct: packet.edgePct,
    tempDiffC: packet.tempDiffC,
    pHEst: packet.pHEst,
    tWoundC: packet.tWoundC,
    tRefC: packet.tRefC,
    ...Object.fromEntries(RAW_CHANNELS.map((c) => [c.key, packet[c.key] ?? null])),
  });
  if (state.history.length > HISTORY_MAX) state.history.shift();
  drawCharts();
}

function buildRawTiles() {
  const wrap = document.getElementById("raw-tiles");
  wrap.innerHTML = "";
  for (const c of RAW_CHANNELS) {
    const tile = document.createElement("article");
    tile.className = "tile";
    tile.innerHTML =
      `<h2>${c.name} <span class="simple">(${c.simple})</span></h2><p class="pin">Feather pin ${c.pin}</p>` +
      `<p class="value"><span id="raw-${c.key}">—</span><span class="unit">mV</span></p>` +
      `<p class="verdict" id="verdict-${c.key}">Waiting for data</p>` +
      `<p class="delta" id="res-${c.key}"></p>` +
      `<p class="delta" id="delta-${c.key}"></p>` +
      `<p class="range">${c.range}</p>`;
    wrap.appendChild(tile);
  }
  const colour = document.createElement("article");
  colour.className = "tile";
  colour.innerHTML =
    `<h2>pH strip reading <span class="simple">(colour sensor)</span></h2><p class="pin">STEMMA QT port · matched against your strip chart</p>` +
    `<div class="swatch" id="col-swatch"></div>` +
    `<p class="value"><span id="col-ph">—</span><span class="unit">pH estimate</span></p>` +
    `<p class="verdict" id="col-verdict">Waiting for data</p>` +
    `<p class="delta" id="col-raw"></p>` +
    `<p class="range" id="range-colour"></p>`;
  wrap.appendChild(colour);
  const live = document.createElement("article");
  live.className = "tile";
  live.innerHTML = `<h2>Live feed</h2><p class="value"><span id="raw-count">0</span><span class="unit">packets</span></p><p class="delta" id="raw-source">Source: not connected</p><p class="delta" id="raw-uptime">Pod on for —</p><p class="delta" id="raw-last">No data yet</p>`;
  wrap.appendChild(live);
  document.getElementById("raw-legend").innerHTML = RAW_CHANNELS
    .map((c) => `<span class="key" style="--c: var(${c.color})">${c.name}${c.dash ? " (dashed)" : ""}</span>`).join("");
}

function updateRaw(packet) {
  state.rawCount = (state.rawCount || 0) + 1;
  document.getElementById("raw-count").textContent = state.rawCount;
  document.getElementById("raw-last").textContent = `Last update ${new Date().toLocaleTimeString()}`;
  document.getElementById("raw-source").textContent =
    state.mode === "mock" ? "Source: MOCK — fake demo data, not the pod" : "Source: LIVE from the pod";
  // timestampMs is the pod's millis(): time since it was last powered on / reset.
  const s = Math.floor(packet.timestampMs / 1000);
  document.getElementById("raw-uptime").textContent = `Pod on for ${Math.floor(s / 60)} min ${s % 60} s`;
  if (packet.tWoundMv == null) return; // older firmware: no raw channels in the packet
  state.rawBaseline ||= Object.fromEntries(RAW_CHANNELS.map((c) => [c.key, packet[c.key]]));
  for (const c of RAW_CHANNELS) {
    const mv = packet[c.key];
    document.getElementById(`raw-${c.key}`).textContent = mv;
    const [text, good] = c.verdict(mv);
    const v = document.getElementById(`verdict-${c.key}`);
    v.textContent = text;
    v.className = `verdict ${good ? "ok" : "bad"}`;
    document.getElementById(`res-${c.key}`).textContent = `${c.rLabel} resistance: ${sensorResistance(mv, c.rFixed, c.sensorOnTop)}`;
    const d = mv - state.rawBaseline[c.key];
    document.getElementById(`delta-${c.key}`).textContent = `Change: ${d > 0 ? "+" : ""}${d} mV`;
  }
}

// Blank every live value. Called on connect, disconnect and mock start, so old readings never
// sit on screen looking live.
function resetLiveView(message) {
  for (const el of [els.vCenter, els.vTemp, els.vPh, els.vLift, els.vHours, els.vTWound, els.vTRef]) {
    el.textContent = "—";
  }
  els.vFaults.textContent = message;
  els.advisory.hidden = true;
  state.rawBaseline = null;
  state.rawCount = 0;
  for (const c of RAW_CHANNELS) {
    document.getElementById(`raw-${c.key}`).textContent = "—";
    const v = document.getElementById(`verdict-${c.key}`);
    v.textContent = message;
    v.className = "verdict";
    document.getElementById(`delta-${c.key}`).textContent = "";
    document.getElementById(`res-${c.key}`).textContent = "";
  }
  document.getElementById("col-swatch").style.background = "transparent";
  document.getElementById("col-ph").textContent = "—";
  document.getElementById("col-verdict").textContent = message;
  document.getElementById("col-verdict").className = "verdict";
  document.getElementById("col-raw").textContent = "";
  document.getElementById("raw-count").textContent = "0";
  document.getElementById("raw-source").textContent = "Source: not connected";
  document.getElementById("raw-uptime").textContent = "Pod on for —";
  document.getElementById("raw-last").textContent = "No data yet";
}

// What the colour reading means, in plain words. Returns [text, good].
function colourVerdict(p) {
  if (p.colC === 0) return ["No reading: colour sensor not detected", false];
  if (p.colMatch === MATCH_NO_READING) return ["Too dark (nothing under it) or glare", false];
  const d = p.colMatch / 10;
  const radius = state.thresholds.phMatchRadius;
  if (d > radius) {
    return [`Not a strip colour (${d.toFixed(1)} pts off the chart; limit ${radius}). Closest is shown.`, false];
  }
  if (!p.colWhiteSet) {
    return [`Rough estimate (${d.toFixed(1)} pts): not calibrated. Hold over white paper and press Calibrate white.`, false];
  }
  const quality = d <= 2 ? "exact" : d <= 5 ? "good" : "rough";
  return [`Calibrated, ${quality} match (${d.toFixed(1)} of ${radius} pts)`, true];
}

function updateColour(packet) {
  if (packet.colC == null) return; // older firmware or mock
  const max = Math.max(packet.colR, packet.colG, packet.colB, 1);
  const k = 255 / max;
  document.getElementById("col-swatch").style.background =
    packet.colC ? `rgb(${Math.round(packet.colR * k)}, ${Math.round(packet.colG * k)}, ${Math.round(packet.colB * k)})` : "transparent";
  document.getElementById("col-ph").textContent = fmt(packet.colNearestPh, 1); // best guess; verdict says how far to trust it
  const [text, good] = colourVerdict(packet);
  const v = document.getElementById("col-verdict");
  v.textContent = text;
  v.className = `verdict ${good ? "ok" : "bad"}`;
  document.getElementById("col-raw").textContent =
    `R ${packet.colR} · G ${packet.colG} · B ${packet.colB} · brightness ${packet.colC}`;
}

function onPacket(packet) {
  updateColour(packet);
  updateRaw(packet);
  updateTiles(packet);
  updateAdvisory(packet);
  pushHistory(packet);
}

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

// series: [{ data, color, min, max, dash }] — each series is scaled to its own range.
// dash: draw dashed, so a line sitting exactly on top of another stays visible.
// Missing points (null / NaN) break the line instead of plotting as zero.
function drawLineChart(canvas, series, labels) {
  const ctx = canvas.getContext("2d");
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth || canvas.width;
  const h = canvas.clientHeight || canvas.height;
  canvas.width = Math.floor(w * dpr);
  canvas.height = Math.floor(h * dpr);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  ctx.strokeStyle = "rgba(26,36,32,0.08)";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = (h * i) / 4;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  if (labels) {
    ctx.fillStyle = cssVar("--muted");
    ctx.font = "11px sans-serif";
    ctx.textAlign = "right";
    ctx.fillText(labels.top, w - 4, 12);
    ctx.fillText(labels.bottom, w - 4, h - 4);
  }

  const n = Math.max(...series.map((s) => s.data.length), 1);
  for (const s of series) {
    ctx.strokeStyle = s.color;
    ctx.lineWidth = 2;
    ctx.setLineDash(s.dash ? [6, 5] : []);
    ctx.beginPath();
    let penDown = false;
    s.data.forEach((v, i) => {
      if (v == null || Number.isNaN(v)) {
        penDown = false;
        return;
      }
      const frac = Math.min(1, Math.max(0, (v - s.min) / (s.max - s.min)));
      const x = n > 1 ? (i / (n - 1)) * (w - 8) + 4 : 4;
      const y = h - frac * (h - 12) - 6;
      if (penDown) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
      penDown = true;
    });
    ctx.stroke();
  }
  ctx.setLineDash([]);
}

function drawCharts() {
  const hist = state.history;
  drawLineChart(
    els.chartMoisture,
    [
      { data: hist.map((p) => p.centerPct), color: cssVar("--chart-a"), min: 0, max: 100 },
    ],
    { top: "100 %", bottom: "0 %" },
  );
  drawLineChart(
    document.getElementById("chart-raw"),
    RAW_CHANNELS.map((c) => ({ data: hist.map((p) => p[c.key]), color: cssVar(c.color), min: 0, max: 3300, dash: c.dash })),
    { top: "3300 mV", bottom: "0 mV" },
  );
  drawLineChart(
    els.chartTemps,
    [
      { data: hist.map((p) => p.tWoundC), color: cssVar("--chart-c"), min: 15, max: 45 },
      { data: hist.map((p) => p.tRefC), color: cssVar("--chart-b"), min: 15, max: 45, dash: true },
    ],
    { top: "45 °C", bottom: "15 °C" },
  );
  drawLineChart(els.chartTempPh, [
    { data: hist.map((p) => p.tempDiffC), color: cssVar("--chart-c"), min: -1, max: 4 },
    { data: hist.map((p) => p.pHEst), color: cssVar("--chart-a"), min: 4, max: 9 },
  ]);
}

// Web Bluetooth works in Chrome/Edge on desktop and Android. Every iPhone/iPad browser uses
// Apple's engine, which has no Web Bluetooth, so say that plainly instead of just failing.
function bluetoothHelpText() {
  const ios = /iPhone|iPad|iPod/.test(navigator.userAgent) ||
    (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
  if (ios) {
    return "iPhone/iPad browsers can't connect to Bluetooth devices from a web page. Use Chrome on Android or a laptop, or the free Bluefy browser app on iPhone. Mock mode works here.";
  }
  if (!window.isSecureContext) {
    return "Bluetooth needs a secure page (https:// or localhost). Open the https link instead. Mock mode works here.";
  }
  return "This browser can't use Bluetooth. Use Chrome or Edge (desktop or Android), or start mock mode.";
}

async function connectBle() {
  if (!navigator.bluetooth) {
    els.conn.textContent = bluetoothHelpText();
    return;
  }
  try {
    els.conn.textContent = "Requesting DressCheck pod…";
    const device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }],
    });
    device.addEventListener("gattserverdisconnected", onDisconnected);
    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE_UUID);
    const chPacket = await service.getCharacteristic(SENSOR_PACKET_UUID);
    const chStatus = await service.getCharacteristic(STATUS_UUID);
    const chThresh = await service.getCharacteristic(THRESHOLDS_UUID);
    const chCmd = await service.getCharacteristic(COMMAND_UUID);

    await chPacket.startNotifications();
    chPacket.addEventListener("characteristicvaluechanged", (ev) => {
      const packet = parseSensorPacket(ev.target.value);
      if (packet) onPacket(packet);
    });

    try {
      const raw = await chThresh.readValue();
      const t = decodeThresholds(raw);
      if (t) {
        state.thresholds = t;
        applyThresholdsToForm(t);
        renderRanges();
      }
    } catch (_) { /* optional */ }

    resetLiveView("Waiting for first packet…");
    state.history = [];
    state.mode = "ble";
    state.device = device;
    state.server = server;
    state.chars = { packet: chPacket, status: chStatus, thresholds: chThresh, command: chCmd };
    els.connect.hidden = true;
    els.mock.hidden = true;
    els.disconnect.hidden = false;
    els.conn.textContent = `Connected to ${device.name || "DressCheck"} — live packets.`;
  } catch (err) {
    els.conn.textContent = `Connect cancelled or failed: ${err.message || err}`;
  }
}

function onDisconnected() {
  resetLiveView("Not connected"); // don't leave the last readings up as if they were live
  if (state.mode === "ble") {
    els.conn.textContent = "Pod disconnected; it's still monitoring and will vibrate if needed.";
  }
  state.mode = "idle";
  state.device = null;
  state.server = null;
  state.chars = {};
  els.connect.hidden = false;
  els.mock.hidden = false;
  els.disconnect.hidden = true;
}

// Web Bluetooth rejects a write while another GATT operation is in flight ("GATT operation already
// in progress"), e.g. a quick press/release of the buzz button. Queue every write so they go one at a time.
let gattQueue = Promise.resolve();
function queueGatt(op) {
  const run = gattQueue.then(op, op);
  gattQueue = run.catch(() => {});
  return run;
}

async function writeCommand(cmd, okText = "Command sent.") {
  if (state.mode === "mock") {
    els.formStatus.textContent = `Mock command ${cmd} accepted (no hardware).`;
    return;
  }
  if (!state.chars.command) {
    els.formStatus.textContent = "Connect a pod first (or use mock mode).";
    return;
  }
  try {
    await queueGatt(() => state.chars.command.writeValue(Uint8Array.of(cmd)));
    els.formStatus.textContent = okText;
  } catch (err) {
    els.formStatus.textContent = `Command failed: ${err.message || err}`;
  }
}

async function saveThresholds(ev) {
  ev.preventDefault();
  const t = readThresholdsFromForm();
  for (const [k, v] of Object.entries(t)) {
    const box = els.form.elements[k];
    if (!box) continue;
    const lo = box.min === "" || box.min == null ? -Infinity : Number(box.min); // blank = no limit
    const hi = box.max === "" || box.max == null ? Infinity : Number(box.max);
    if (!Number.isFinite(v) || v < lo || v > hi) {
      els.formStatus.textContent = `${k} must be between ${box.min} and ${box.max}.`;
      return;
    }
  }
  if (t.saturationSoonPct >= t.saturationNowPct) {
    els.formStatus.textContent = "“Saturation soon” must be lower than “Saturation now”.";
    return;
  }
  if (state.mode === "mock") {
    state.thresholds = t;
    renderRanges();
    els.formStatus.textContent = "Thresholds saved locally (mock).";
    return;
  }
  if (!state.chars.thresholds) {
    els.formStatus.textContent = "Connect a pod first (or use mock mode).";
    return;
  }
  try {
    await queueGatt(() => state.chars.thresholds.writeValue(encodeThresholds(t)));
    state.thresholds = t;
    renderRanges();
    els.formStatus.textContent = "Thresholds written to pod flash.";
  } catch (err) {
    els.formStatus.textContent = `Pod rejected thresholds: ${err.message || err}`;
  }
}

// Inverse of the firmware's tempFromMv(): 3V -> 10k -> node -> 10k NTC (Beta 3950) -> GND.
function mvFromTempC(c) {
  const rNtc = 10000 * Math.exp(3950 * (1 / (c + 273.15) - 1 / 298.15));
  return Math.round((3300 * rNtc) / (10000 + rNtc));
}

function stopMock() {
  if (state.mockTimer) {
    clearInterval(state.mockTimer);
    state.mockTimer = null;
  }
}

function startMock() {
  stopMock();
  state.mode = "mock";
  state.mockT = 0;
  state.history = [];
  resetLiveView("Waiting for first packet…");
  els.connect.hidden = true;
  els.mock.hidden = true;
  els.disconnect.hidden = false;
  els.conn.textContent = "Mock mode — fake SENSOR_PACKET stream (no hardware).";
  els.formStatus.textContent = "";

  state.mockTimer = setInterval(() => {
    state.mockT += 1;
    const t = state.mockT;
    // Ramp moisture: centre rises, edge later; soft advisory near end
    const center = Math.min(95, 18 + t * 1.4 + Math.sin(t / 4) * 2);
    const edge = Math.max(0, center - 35 + Math.sin(t / 5));
    const tempDiff = 0.4 + Math.sin(t / 8) * 0.2 + (t > 50 ? 0.3 : 0);
    const pHEst = 6.8 + Math.min(0.9, t / 80);
    const lift = t > 70 ? 0.3 : 0.05 + Math.random() * 0.02;
    const hoursWorn = t / 12;

    const reasons = [];
    if (center >= state.thresholds.saturationNowPct) reasons.push(4);
    else if (lift >= state.thresholds.liftThreshold) reasons.push(5);
    else if (center >= state.thresholds.saturationSoonPct) reasons.push(7);

    onPacket({
      timestampMs: t * 1000,
      centerPct: center,
      edgePct: edge,
      tempDiffC: tempDiff,
      tWoundC: 33.5 + tempDiff,
      tRefC: 33.5 - 9.5,
      centerMv: Math.round(142 + center * 25),
      edgeMv: Math.round(142 + edge * 25),
      liftMv: Math.round(320 + lift * 800),
      // Mock colour sensor: a calibrated strip whose colour drifts mustard -> olive as pH rises.
      colR: Math.round(1150 - (pHEst - 6.8) * 300),
      colG: Math.round(1050 - (pHEst - 6.8) * 120),
      colB: Math.round(260 + (pHEst - 6.8) * 400),
      colC: 2600,
      colMatch: 18,
      colNearestPh: pHEst,
      colWhiteSet: true,
      tWoundMv: mvFromTempC(33.5 + tempDiff),
      tRefMv: mvFromTempC(33.5 - 9.5),
      pHEst,
      lift,
      hoursWorn,
      faults: 0,
      urgency: reasons.length ? (reasons.includes(7) ? 1 : 2) : 0, // 7 = saturation soon
      reasons,
    });
  }, 1000);
}

function disconnectAll() {
  stopMock();
  if (state.device?.gatt?.connected) {
    state.device.gatt.disconnect();
  }
  onDisconnected();
  els.conn.textContent = "Disconnected. Connect a pod or start mock mode.";
}

els.connect.addEventListener("click", connectBle);
els.mock.addEventListener("click", startMock);
els.disconnect.addEventListener("click", disconnectAll);
els.form.addEventListener("submit", saveThresholds);
els.newDressing.addEventListener("click", () => writeCommand(CMD.NEW_DRESSING));
els.ack.addEventListener("click", () => writeCommand(CMD.ACK_ALERT));
els.selfTest.addEventListener("click", () => writeCommand(CMD.SELF_TEST));
document.getElementById("btn-white").addEventListener("click", () => writeCommand(CMD.CALIBRATE_WHITE));

// Hold-to-buzz: motor runs while the button (or Space/Enter on it) is held.
const buzzBtn = document.getElementById("btn-buzz");
let buzzing = false;
function buzzStart(ev) {
  ev.preventDefault();
  if (buzzing) return;
  buzzing = true;
  buzzBtn.textContent = "Buzzing…";
  writeCommand(CMD.BUZZ_ON, "Buzzing (release to stop, 5 s max).");
}
function buzzStop() {
  if (!buzzing) return;
  buzzing = false;
  buzzBtn.textContent = "Hold to buzz";
  writeCommand(CMD.BUZZ_OFF, "Buzz stopped.");
}
buzzBtn.addEventListener("pointerdown", buzzStart);
buzzBtn.addEventListener("contextmenu", (ev) => ev.preventDefault()); // long-press on phones
for (const ev of ["pointerup", "pointerleave", "pointercancel", "blur"]) buzzBtn.addEventListener(ev, buzzStop);
buzzBtn.addEventListener("keydown", (ev) => { if ((ev.key === " " || ev.key === "Enter") && !ev.repeat) buzzStart(ev); });
buzzBtn.addEventListener("keyup", (ev) => { if (ev.key === " " || ev.key === "Enter") buzzStop(); });

buildRawTiles();
addSliders();
applyThresholdsToForm(state.thresholds);
renderRanges();
drawCharts();
window.addEventListener("resize", drawCharts);

// Tell phone users up front if Bluetooth can't work here.
if (!navigator.bluetooth || !window.isSecureContext) els.conn.textContent = bluetoothHelpText();
// ...?mock in the address opens straight into demo data (handy for showing it on any phone).
if (new URLSearchParams(location.search).has("mock")) startMock();
