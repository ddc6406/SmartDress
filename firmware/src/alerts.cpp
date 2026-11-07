#include "alerts.h"
#include "pins.h"
#include <Adafruit_NeoPixel.h>

namespace {
  Adafruit_NeoPixel pixel(1, Pins::NEOPIXEL, NEO_GRB + NEO_KHZ800);
  uint32_t lastPulseMs = 0;
  bool motorOn = false;
  uint32_t motorUntilMs = 0;
  uint32_t btnDownMs = 0;
  bool btnWasDown = false;
  bool longFired = false;

  void motorWrite(bool on) {
    digitalWrite(Pins::MOTOR, on ? HIGH : LOW);
    motorOn = on;
  }
}  // namespace

void alertsInit() {
  pixel.begin();
  pixel.setBrightness(40);
  pixel.clear();
  pixel.show();
  motorWrite(false);
}

void setNeoPixelRGB(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void stopVibration() {
  motorWrite(false);
  motorUntilMs = 0;
}

void triggerAlert(Urgency urgency) {
  lastPulseMs = millis();  // repeat-haptic timer counts from the moment the alert fires
  switch (urgency) {
    case Urgency::OK:
      setNeoPixelRGB(0, 180, 40);  // green on-device only
      stopVibration();
      break;
    case Urgency::CHANGE_SOON:
      setNeoPixelRGB(220, 180, 0);
      motorWrite(true);
      motorUntilMs = millis() + 120;
      break;
    case Urgency::CHANGE_NOW:
      setNeoPixelRGB(220, 30, 30);
      motorWrite(true);
      motorUntilMs = millis() + 150;
      break;
    case Urgency::CONTACT_CLINIC:
      setNeoPixelRGB(140, 40, 200);
      motorWrite(true);
      motorUntilMs = millis() + 400;
      break;
    case Urgency::SENSOR_FAULT:
      setNeoPixelRGB(255, 120, 0);
      motorWrite(true);
      motorUntilMs = millis() + 500;
      break;
  }
}

void alertsTick(Urgency urgency, bool acknowledged) {
  const uint32_t now = millis();
  if (motorOn && now >= motorUntilMs) {
    motorWrite(false);
  }

  // Repeat haptic until ack for urgent states (on-device)
  if (!acknowledged) {
    if (urgency == Urgency::CHANGE_NOW && now - lastPulseMs > 10UL * 60UL * 1000UL) {
      lastPulseMs = now;
      motorWrite(true);
      motorUntilMs = now + 150;
    } else if (urgency == Urgency::CONTACT_CLINIC &&
               now - lastPulseMs > 30UL * 60UL * 1000UL) {
      lastPulseMs = now;
      motorWrite(true);
      motorUntilMs = now + 400;
    }
  }

  // Orange blink for sensor fault
  if (urgency == Urgency::SENSOR_FAULT) {
    bool on = ((now / 400) % 2) == 0;
    setNeoPixelRGB(on ? 255 : 40, on ? 120 : 20, 0);
  }
}

bool buttonPressed() {
  const bool down = digitalRead(Pins::USER_BTN) == LOW;  // active low typical
  bool edged = false;
  if (down && !btnWasDown) {
    btnDownMs = millis();
    longFired = false;
  }
  if (!down && btnWasDown && !longFired && (millis() - btnDownMs) < 800) {
    edged = true;
  }
  btnWasDown = down;
  return edged;
}

bool buttonLongPressed() {
  const bool down = digitalRead(Pins::USER_BTN) == LOW;
  if (down && !longFired && (millis() - btnDownMs) >= 1500) {
    longFired = true;
    return true;
  }
  return false;
}
