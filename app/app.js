/**
 * DressCheck Web Bluetooth dashboard: live readings, trends, and plain-language advisories.
 * Packet / threshold layouts mirror firmware/include/ble_service.h and ble_service.cpp.
 */

const SERVICE_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000001";
const SENSOR_PACKET_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000002";
const STATUS_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000003";
const THRESHOLDS_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000004";
const COMMAND_UUID = "dcc00001-a1b2-c3d4-e5f6-000000000005";

const CMD = { NEW_DRESSING: 1, ACK_ALERT: 2, SELF_TEST: 3 };

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
  vEdge: document.getElementById("v-edge"),
  vTemp: document.getElementById("v-temp"),
  vPh: document.getElementById("v-ph"),
  vLift: document.getElementById("v-lift"),
  vHours: document.getElementById("v-hours"),
  chartMoisture: document.getElementById("chart-moisture"),
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
  },
  mockTimer: null,
  mockT: 0,
};

function fmt(n, digits = 1) {
  if (n == null || Number.isNaN(n)) return "—";
  return Number(n).toFixed(digits);
}

const SENSOR_PACKET_LEN = 22;
const THRESHOLDS_LEN = 30;

// view: DataView over the characteristic value.
function parseSensorPacket(view) {
  if (view.byteLength < SENSOR_PACKET_LEN) return null;
  const reasons = [view.getUint8(18), view.getUint8(19), view.getUint8(20), view.getUint8(21)]
    .filter((c) => c !== 0);
  return {
    timestampMs: view.getUint32(0, true),
    centerPct: view.getUint16(4, true) / 10,
    edgePct: view.getUint16(6, true) / 10,
    tempDiffC: view.getInt16(8, true) / 100,
    pHEst: view.getUint16(10, true) / 100 || null, // firmware sends 0 when pH is unavailable
    lift: view.getUint16(12, true) / 1000,
    hoursWorn: view.getUint16(14, true) / 10,
    faults: view.getUint8(16),
    urgency: view.getUint8(17),
    reasons,
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
  };
}

function applyThresholdsToForm(t) {
  const f = els.form;
  for (const [k, v] of Object.entries(t)) {
    if (f.elements[k]) f.elements[k].value = v;
  }
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
  };
}

function updateTiles(packet) {
  els.vCenter.textContent = fmt(packet.centerPct, 1);
  els.vEdge.textContent = fmt(packet.edgePct, 1);
  els.vTemp.textContent = fmt(packet.tempDiffC, 2);
  els.vPh.textContent = fmt(packet.pHEst, 2);
  els.vLift.textContent = fmt(packet.lift, 2);
  els.vHours.textContent = fmt(packet.hoursWorn, 1);
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
  });
  if (state.history.length > HISTORY_MAX) state.history.shift();
  drawCharts();
}

function onPacket(packet) {
  updateTiles(packet);
  updateAdvisory(packet);
  pushHistory(packet);
}

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

// series: [{ data, color, min, max }] — each series is scaled to its own range.
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
}

function drawCharts() {
  const hist = state.history;
  drawLineChart(
    els.chartMoisture,
    [
      { data: hist.map((p) => p.centerPct), color: cssVar("--chart-a"), min: 0, max: 100 },
      { data: hist.map((p) => p.edgePct), color: cssVar("--chart-b"), min: 0, max: 100 },
    ],
    { top: "100 %", bottom: "0 %" },
  );
  drawLineChart(els.chartTempPh, [
    { data: hist.map((p) => p.tempDiffC), color: cssVar("--chart-c"), min: -1, max: 4 },
    { data: hist.map((p) => p.pHEst), color: cssVar("--chart-a"), min: 4, max: 9 },
  ]);
}

async function connectBle() {
  if (!navigator.bluetooth) {
    els.conn.textContent = "Web Bluetooth unavailable. Use Chrome on desktop/Android, or start mock mode.";
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
      }
    } catch (_) { /* optional */ }

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
  els.advisory.hidden = true; // don't leave the last advisory up as if it were live
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

async function writeCommand(cmd) {
  if (state.mode === "mock") {
    els.formStatus.textContent = `Mock command ${cmd} accepted (no hardware).`;
    return;
  }
  if (!state.chars.command) {
    els.formStatus.textContent = "Connect a pod first (or use mock mode).";
    return;
  }
  try {
    await state.chars.command.writeValue(Uint8Array.of(cmd));
    els.formStatus.textContent = "Command sent.";
  } catch (err) {
    els.formStatus.textContent = `Command failed: ${err.message || err}`;
  }
}

async function saveThresholds(ev) {
  ev.preventDefault();
  const t = readThresholdsFromForm();
  if (t.saturationSoonPct >= t.saturationNowPct) {
    els.formStatus.textContent = "“Saturation soon” must be lower than “Saturation now”.";
    return;
  }
  if (state.mode === "mock") {
    state.thresholds = t;
    els.formStatus.textContent = "Thresholds saved locally (mock).";
    return;
  }
  if (!state.chars.thresholds) {
    els.formStatus.textContent = "Connect a pod first (or use mock mode).";
    return;
  }
  try {
    await state.chars.thresholds.writeValue(encodeThresholds(t));
    state.thresholds = t;
    els.formStatus.textContent = "Thresholds written to pod flash.";
  } catch (err) {
    els.formStatus.textContent = `Pod rejected thresholds: ${err.message || err}`;
  }
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
    if (edge >= state.thresholds.edgeWetPct) reasons.push(3);
    else if (center >= state.thresholds.saturationNowPct) reasons.push(4);
    else if (lift >= state.thresholds.liftThreshold) reasons.push(5);
    else if (center >= state.thresholds.saturationSoonPct) reasons.push(7);

    onPacket({
      timestampMs: t * 1000,
      centerPct: center,
      edgePct: edge,
      tempDiffC: tempDiff,
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

applyThresholdsToForm(state.thresholds);
drawCharts();
window.addEventListener("resize", drawCharts);
