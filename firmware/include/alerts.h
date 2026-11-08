#pragma once

#include "decision.h"

void alertsInit();
void triggerAlert(Urgency urgency);
void stopVibration();
void pulseMotor(uint32_t ms);  // bench test: buzz the motor for ms
void setNeoPixelRGB(uint8_t r, uint8_t g, uint8_t b);
void alertsTick(Urgency urgency, bool acknowledged);
bool buttonPressed();
bool buttonLongPressed();
