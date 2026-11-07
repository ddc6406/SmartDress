#pragma once

// DressCheck pin map — Adafruit Feather ESP32 V2 (ADC1 only for sensing)

namespace Pins {
  constexpr int EXC = 26;           // Moisture excitation (pulse HIGH ~5 ms)
  constexpr int MOTOR = 25;         // Direct NPN base drive via 1k (no driver board)
  constexpr int M_CENTER = 34;      // Centre moisture ADC1 (input-only)
  constexpr int M_EDGE = 39;        // Edge moisture ADC1 (input-only)
  constexpr int T_WOUND = 36;       // Wound thermistor ADC1 (input-only)
  constexpr int T_REF = 37;         // Reference thermistor ADC1 (input-only)
  constexpr int LIFT = 33;          // Flex / lift ADC1
  constexpr int NEOPIXEL = 0;       // Onboard NeoPixel data
  constexpr int NEOPIXEL_EXT = 14;  // Optional Flora NeoPixel
  constexpr int NEOPIXEL_POWER = 2; // NeoPixel + STEMMA QT power enable
  constexpr int USER_BTN = 38;      // Built-in user button (avoid Arduino BUTTON macro)
  constexpr int I2C_SDA = 22;
  constexpr int I2C_SCL = 20;
}

void pinsInit();
