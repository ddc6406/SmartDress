# DressCheck sensor assumptions

Prototype conversions used by the pod firmware and mirrored in the web app.
**Non-diagnostic:** values are compared to clinician-set thresholds only. The device never diagnoses infection, healing stage, or wound condition.

---

## Moisture (centre + edge)

**Circuit:** EXC (GPIO26) pulsed HIGH for ~5 ms → conductive-thread electrode A (shared) → fabric → electrode B → sense node → **100 kΩ pulldown to GND**. Sense nodes on ADC1 (centre GPIO34, edge GPIO39).

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

**Caveat (unverified on hardware):** with a 100 kΩ pulldown, an open thread and a bone-dry pad both read ≈0 mV, so the `< 20 mV` “open” fault can fire on a genuinely dry dressing. Check the dry reading with `SELF_TEST` before relying on it.

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

**Normalization:** at `NEW_DRESSING`, capture a flat (sealed) millivolt reading. Map bend to 0…1:

\[
\mathrm{lift} = \mathrm{clamp}\!\left(\frac{V - V_{\mathrm{flat}}}{V_{\mathrm{maxbend}} - V_{\mathrm{flat}}},\ 0,\ 1\right)
\]

Default clinician lift threshold is **0.25** (normalised). Exact flex resistance is event-calibratable; firmware stores flat / max-bend session values in flash.

---

## Colour → estimated pH (universal indicator ~4–9)

**Sensor:** STEMMA TCS34725 (assumed present). LED on briefly (~50 ms), RGB normalised by clear channel, then hue → pH lookup.

**Framing:** this is an **estimated pH from indicator colour**, not a lab pH meter. It is provisional and **event-calibratable** against known buffers / strips at the hackathon.

### Provisional hue → pH table (universal indicator ≈4–9)

| Approx. hue (°) | Colour appearance | Estimated pH |
| --- | --- | ---: |
| 0–20 | Red / pink | 4.0 |
| 20–40 | Red–orange | 4.5 |
| 40–60 | Orange | 5.0 |
| 60–80 | Yellow–orange | 5.5 |
| 80–100 | Yellow | 6.0 |
| 100–130 | Yellow–green | 6.5 |
| 130–160 | Green | 7.0 |
| 160–190 | Blue–green | 7.5 |
| 190–220 | Blue | 8.0 |
| 220–260 | Blue–violet | 8.5 |
| 260–300 | Violet | 9.0 |

Hue outside this band, a saturated clear channel (≥ 20000 counts at 50 ms integration; the sensor tops out at 21504), or a missing I²C device → “pH unavailable,” not a fabricated number. Colour faults are reported in the packet’s `faults` byte but never raise `SENSOR_FAULT` urgency, so a missing colour sensor cannot suppress the moisture, temperature, or lift alerts.

Default clinician pH alert is **7.5**, sustained (default 30 min) before a “contact clinic for review” advisory.

---

## Soft advisories (app + pod)

On-device NeoPixel and motor keep urgency cues. The web dashboard shows **live readings, trends, and plain-language advisories** derived from reason codes — not coloured OK / CHANGE_SOON status cards. Example copy:

- “Your clinician’s criteria say it’s time to change your dressing.”
- “Contact your clinic for review.”
- “Dressing fluid is reaching the edge — leak may be likely.”
