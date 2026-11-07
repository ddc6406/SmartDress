# DressCheck

Smart wound-dressing monitor for **natHacks 2026** (Edmonton · Standard Division · Healthcare & Accessibility).

A disposable sensing dressing plugs into a reusable pod. The pod watches moisture, temperature, optional pH colour, and lift. Decision logic and haptic/LED alerts run on the device so they still fire if the phone drops. A Web Bluetooth app (Chrome) shows live readings and trends (not discrete alert statuses) and can let a clinician set thresholds.

**Prototype only.** Tested on a skin-phantom rig. No real wounds. The device never diagnoses — it compares readings to clinician-set criteria and surfaces messages like “your clinician’s criteria say it’s time to change your dressing” or “contact your clinic for review.”

---

## What’s in the box (prototype)

| Piece | Role |
| --- | --- |
| Sensing dressing | Moisture electrodes (centre + edge), thermistors, optional pH patch, flex/lift |
| DressCheck pod | Adafruit Feather ESP32 V2 + dividers, motor, BLE |
| Phone / laptop | Chrome Web Bluetooth clinician / patient UI |

Hardware detail and breadboard steps live in the team build guide. Sensor conversion assumptions are documented in [`docs/sensor-assumptions.md`](docs/sensor-assumptions.md).

---

## Architecture

![DressCheck pod block diagram](docs/schematics/pod-block-diagram.png)

Feather ESP32 V2 does ADC1 sensing and BLE out. Moisture EXC is pulsed (~5 ms). Decision logic stays on the pod so alerts still fire if the phone disconnects.

**BLE:** one custom service (`dcc00001-a1b2-c3d4-e5f6-000000000001`)

- `SENSOR_PACKET` — notify  
- `STATUS` — notify  
- `THRESHOLDS` — write  
- `COMMAND` — write (`NEW_DRESSING`, `ACK_ALERT`, `SELF_TEST`)

---

## Repo layout

```
/
├── firmware/          # PlatformIO / Arduino for Feather ESP32 V2
├── app/               # Static Web Bluetooth UI (Chrome)
├── docs/
│   ├── schematics/    # PNG diagrams used in this README
│   └── sensor-assumptions.md
└── README.md
```

---

## Hardware quick map

Pod: **Adafruit Feather ESP32 V2** (3.3 V logic only). ADC1 only for sensing.

| Job | GPIO | Notes |
| --- | --- | --- |
| Moisture EXC | 26 | Pulse HIGH ~5 ms, then LOW |
| Motor | 25 | Via 1k → NPN (direct; no driver board); flyback diode |
| Centre moisture | 34 | 100k to GND |
| Edge moisture | 39 | 100k to GND |
| Wound thermistor | 36 | 10k to 3V |
| Reference thermistor | 37 | 10k to 3V |
| Flex (lift) | 33 | 47k to 3V |
| Colour sensor | SDA 22 / SCL 20 | STEMMA QT (TCS34725 assumed) |
| NeoPixel | 0 (onboard) / 14 optional | Power enable GPIO2 if needed |
| Ack button | 38 | Built-in |

### Breadboard layout

![Feather + divider columns on breadboard](docs/schematics/breadboard-layout.png)

Mini / half-size two-board layout: Feather on the left (USB-C facing left), divider columns one per sensor on the right. 3V top rail, GND bottom rail.

### Sensor dividers

![Thermistor, moisture, and flex divider circuits](docs/schematics/sensor-dividers.png)

- Thermistors: 3V → 10k → node (GPIO36 / GPIO37) → NTC → GND  
- Moisture zones: EXC → thread → node (GPIO34 / GPIO39) → 100k → GND  
- Flex: 3V → 47k → node (GPIO33) → flex → GND  

### Motor circuit

![Vibration motor with NPN and flyback diode](docs/schematics/motor-circuit.png)

GPIO25 → 1k → NPN base. Motor red wire to 3V. Diode across the motor (stripe / cathode to 3V). Emitter to GND.

---

## Setup

### Firmware

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Open `firmware/`.
3. Board is already selected: **Adafruit ESP32 Feather V2** (`adafruit_feather_esp32_v2`).
4. Connect USB-C, build & upload.
5. Serial monitor @ **115200** for self-test logs.

```bash
cd firmware
pio run
pio run -t upload
pio device monitor -b 115200
```

Libraries pulled by PlatformIO (`platformio.ini`):

- [Adafruit TCS34725](https://github.com/adafruit/Adafruit_TCS34725)
- [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)
- ESP32 Arduino BLE (`BLEDevice` / GATT)

### Web app

1. Use **Chrome** (Web Bluetooth; Safari not supported).
2. Serve `app/` over HTTPS or `localhost`.
3. Pair the pod, or click **Mock mode** to exercise the UI without hardware.
4. Dashboard shows live tiles + trends and a soft advisory banner from reason codes — not coloured OK / CHANGE_SOON status cards. On-device NeoPixel / motor keep alerts.

```bash
cd app
python3 -m http.server 8765
# then open http://127.0.0.1:8765
#
# or: npx --yes serve -l 8765 .
```

---

## Hardware validation checklist

Run through this once on the real rig before the demo. The firmware has only been compile-checked against stubs, not run on the pod.

**Build**
- [ ] `pio run` succeeds. If `ble_service.cpp` fails on `getValue()` types, pin the `espressif32` platform version in `platformio.ini`.
- [ ] Serial monitor at 115200 shows `=== DressCheck pod ===` and `TCS34725 present` (or the missing-sensor line if it is unplugged).

**Sensors (dry dressing, send `SELF_TEST`)**
- [ ] Dry moisture reads above 20 mV on both zones. At or below 20 mV the pod reports a moisture fault on a healthy dry pad; if so, relax the open-circuit check in `checkFaults()`.
- [ ] Wet the pad: centre/edge mV rise and the percentage climbs toward 100.
- [ ] Both thermistors read plausible room temperature (about 15–35 °C), and warming one shows a difference.
- [ ] Unplug a thermistor: `faults` becomes non-zero and the pod goes to sensor fault.
- [ ] Bend the flex sensor: lift rises from 0 toward 1.
- [ ] Colour sensor: clear-channel count stays below 20000 with the LED on. Check the hue-to-pH table against a known buffer or strip.

**Behaviour**
- [ ] Boot with an active session: no orange fault flash and no buzz in the first 10 s.
- [ ] Long-press the button (about 1.5 s) or send `NEW_DRESSING`: blue LED, then green, no immediate red alert.
- [ ] Change dressings after a wet one: readings restart from dry, not from the old values.
- [ ] Reboot mid-session: the log says `session restored` with a sensible hours-worn figure, not hundreds of hours.
- [ ] Unplug the colour sensor: moisture alerts still fire.

**Alerts and BLE**
- [ ] Each urgency shows its colour and vibration pattern; a short button press silences the motor.
- [ ] Chrome connects and live packets arrive. If tiles stay blank, the MTU is too small (packet is 22 bytes).
- [ ] Save thresholds from the app, power-cycle the pod, reconnect: values persisted. An out-of-range value (for example soon above now) is rejected.
- [ ] Disconnect the phone: the pod still alerts on its own.

---

## Team & credits

**Team:** Tirthrajsinh (Tirth) Gohil, Mehtab Singh, Om Arey, Shreyas Prassana, Dhairya Chauhan  
**Hardware:** Adafruit Feather ESP32 V2, NTC thermistors (Beta 3950), conductive-thread moisture pad, flex sensor, STEMMA TCS34725 colour sensor, vibration motor (direct NPN drive)  

**Libraries / assets:** PlatformIO (Espressif32), Adafruit TCS34725, Adafruit NeoPixel, ESP32 Arduino BLE stack, team schematic PNGs in `docs/schematics/`

---

## Status

Working prototype path: breadboard pod + phantom dressing. Schematics for the current divider / motor layout are the team’s active setup. Firmware (`firmware/`) and static Web Bluetooth app (`app/`) are in-repo for the event build.
