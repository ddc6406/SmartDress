// Heartbeat for the Adafruit Feather ESP32 V2: blinks the onboard NeoPixel (green) and the red
// LED on pin 13 once a second while the board has power. No power = no light.
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

constexpr int NEOPIXEL_PIN = 0;        // onboard NeoPixel data
constexpr int NEOPIXEL_POWER_PIN = 2;  // must be HIGH to power the NeoPixel
constexpr int RED_LED_PIN = 13;

Adafruit_NeoPixel pixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  Serial.begin(115200);
  pinMode(NEOPIXEL_POWER_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_POWER_PIN, HIGH);
  pinMode(RED_LED_PIN, OUTPUT);
  pixel.begin();
  pixel.setBrightness(40);
  Serial.println("heartbeat: running");
}

void loop() {
  static bool on = false;
  on = !on;
  pixel.setPixelColor(0, on ? pixel.Color(0, 200, 40) : 0);
  pixel.show();
  digitalWrite(RED_LED_PIN, on ? HIGH : LOW);
  Serial.println(on ? "heartbeat: on" : "heartbeat: off");
  delay(500);
}
