#include "pins.h"
#include <Arduino.h>

void pinsInit() {
  pinMode(Pins::EXC, OUTPUT);
  digitalWrite(Pins::EXC, LOW);  // never leave excitation on

  pinMode(Pins::MOTOR, OUTPUT);
  digitalWrite(Pins::MOTOR, LOW);

  pinMode(Pins::NEOPIXEL_POWER, OUTPUT);
  digitalWrite(Pins::NEOPIXEL_POWER, HIGH);  // power NeoPixel + STEMMA QT

  pinMode(Pins::USER_BTN, INPUT_PULLUP);

  // ADC1 sense pins are input-only on ESP32; analogRead configures them
  analogReadResolution(12);
}
