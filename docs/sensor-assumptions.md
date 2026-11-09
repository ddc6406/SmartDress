# DressCheck sensor assumptions

Prototype conversions used by the pod firmware and mirrored in the web app.
**Non-diagnostic:** values are compared to clinician-set thresholds only. The device never diagnoses infection, healing stage, or wound condition.

---

## Moisture (single zone, two threads)

**Circuit:** two conductive threads taped on a gauze pad. Thread 1 is driven by EXC (A0 / GPIO26), pulsed HIGH for ~5 ms. Thread 2 goes to the sense node (A2 / GPIO34), which has a **100 kΩ pulldown to GND**. Dry gauze doesn't conduct (node ≈ 0 V); water bridges the threads and the node voltage rises (measured on the real pad: 142 mV dry, ~3040 mV soaked, back to 142 mV when dry). The edge zone (A3 / GPIO39) is not fitted; set `Pins::EDGE_ZONE_FITTED` to `true` if a third thread is added.

**Why pulse:** Constant DC electrolyses the thread and drifts readings. Firmware sets EXC HIGH, averages ADC samples, then immediately drives EXC LOW.

**Conversion to %:**

1. Read millivolts with `analogReadMilliVolts` while EXC is high.
2. Wetter pad → lower thread resistance → higher node voltage.
3. Map against session baselines captured at `NEW_DRESSING`:

\[
\text{pct} = \mathrm{clamp}\!\left(\frac{V - V_{\mathrm{dry}}}{V_{\mathrm{sat}} - V_{\mathrm{dry}}} \times 100,\ 0,\ 100\right)
\]

- \(V_{\mathrm{dry}}\): dry baseline for that zone at dressing apply.
- \(V_{\mathrm{sat}}\): provisional saturated calibration (~2800 mV on the phantom; event-calibratable).

Open/short (≈0 mV or ≈3300 mV) is treated as a sensor fault, not a moisture reading.

**Measured on the real board:** the ESP32 ADC reads about 140 mV even at 0 V (unwired moisture pins both read 142 mV). It also tops out at about 3150 mV at 3.3 V. Fault limits therefore use **≤ 150 mV = at ground** and **≥ 3100 mV = at 3.3 V**. A dry pad legitimately reads ≈0 V, so an open thread looks the same as a dry pad; only a moisture node stuck at 3.3 V (shorted) is flagged. Thermistor readings outside 150–3100 mV are reported as *no reading* rather than converted (they would otherwise show impossible values like 116 °C or −30 °C).

---

## Temperature (NTC Beta 3950)

**Circuit:** 3.3 V → **10 kΩ** → node (wound GPIO36 / reference GPIO37) → **10 kΩ NTC** → GND.

**Assumptions:**

| Parameter | Value |
| --- | --- |
| \(R_0\) | 10 kΩ at 25 °C |
| Beta | **3950** (MF52D-type parts in hand) |
| Series resistor | 10 kΩ |

**Conversion:**

\[
R_{\mathrm{ntc}} = 10\,\mathrm{k}\Omega \times \frac{V_{\mathrm{mV}}}{3300 - V_{\mathrm{mV}}}
\]

\[
T(^\circ\mathrm{C}) = \frac{1}{\frac{1}{298.15} + \frac{\ln(R_{\mathrm{ntc}}/R_0)}{\beta}} - 273.15
\]

Wound-vs-skin difference uses a thermistor offset measured at new-dressing calibration, then a settled baseline after ~20 minutes on the phantom/body. An open or shorted thermistor is a `SENSOR_FAULT`, not a zero difference. Wear time survives a reboot: elapsed time is checkpointed to flash every 5 min and `startMs` is rebuilt from it. Infection-related temperature flags require a **sustained** rise (default 30 min), never a single sample.

---

## Flex / lift

**Circuit:** 3.3 V → **47 kΩ** → node (GPIO33) → flex sensor → GND.

**Measured on the built pod:** the pin reads **~3100 mV lying flat** (normal, not a fault) and the voltage **drops as it bends**. The firmware reports it **flipped** (`liftMv = 3300 − pin mV`), so flat reads ~200 mV and bending makes the number rise. Only a pin at ~0 V (flipped 3150+; no 3V feed / shorted) is flagged as a sensor fault.

**Normalization:** at `NEW_DRESSING`, capture a flat (sealed) millivolt reading. Map bend to 0…1:

\[
\mathrm{lift} = \mathrm{clamp}\!\left(\frac{V - V_{\mathrm{flat}}}{V_{\mathrm{peel}} - V_{\mathrm{flat}}},\ 0,\ 1\right)
\]

where V is the flipped reading and full scale = peeling level − flat. The **peeling level is 650 mV** (flipped), because peeling was measured at ~600–700 mV; it's adjustable as "Peeling level" in the dashboard settings. The lift alert fires when the flex reaches the peeling level (lift = 1.0). Exact flex resistance is event-calibratable; firmware stores flat / max-bend session values in flash.

---

## Colour → estimated pH (universal indicator ~4–9)

**Sensor:** STEMMA TCS34725 on the QT port. Its white LED switches on for ~0.1 s per reading (every 2 s on the debug build, every 60 s otherwise) and is off the rest of the time.

**Framing:** this is an **estimated pH from indicator-strip colour**, not a lab pH meter.

### How a reading becomes a pH

1. **White reference (once per setup).** Hold the sensor 3–5 mm over plain white paper (the chart's white border) and press **Calibrate white** on the dashboard (serial `w` on bench builds). Each raw channel is divided by its white value, which removes the sensor's own colour bias (its green channel is naturally strong). Saved in flash. Until it is set, no pH is reported.
2. **Colour + brightness.** The white-balanced reading becomes chromaticity (colour independent of brightness) plus brightness relative to white.
3. **Match against the team's strip chart.** The 14 squares of the pH 1–14 chart (sampled from a photo, white-balanced against the chart's own paper) form a path 1 → 2 → … → 14 through colour space. The reading is mapped to the **closest point on that path**, so a colour between two squares reads as an in-between pH (e.g. halfway between 7 and 8 → 7.5).
4. **Leniency: 8 colour points.** If the reading is more than 8 points off the path it is *not a strip colour* (white paper ≈ 20, skin ≈ 11) and no pH is reported.

Why a path and brightness: on this chart pH 9 (dark olive) has almost the same colour as the 7 → 8 transition and differs mainly in darkness. Nearest-square matching would read 7.5 as 9. Brightness is weighted (×20) because the sensor sits at a fixed distance in the pod; tested to stay within ±0.5 pH for ±15 % brightness drift and ±2-point colour noise across pH 6–9.

**Caveat:** the references come from a phone photo, not from the sensor itself. Verify with real dipped strips (tap water ≈ 7, vinegar ≈ 3) before trusting decimals.

Default clinician pH alert is **7.5**, sustained (default 30 min) before a “contact clinic for review” advisory.

---

## Soft advisories (app + pod)

On-device NeoPixel and motor keep urgency cues. The web dashboard shows **live readings, trends, and plain-language advisories** derived from reason codes — not coloured OK / CHANGE_SOON status cards. Example copy:

- “Your clinician’s criteria say it’s time to change your dressing.”
- “Contact your clinic for review.”
- “Dressing fluid is reaching the edge — leak may be likely.”
