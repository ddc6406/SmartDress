#pragma once

#include "decision.h"

void alertsInit();
void triggerAlert(Urgency urgency);
void stopVibration();
void setNeoPixelRGB(uint8_t r, uint8_t g, uint8_t b);
void alertsTick(Urgency urgency, bool acknowledged);
bool buttonPressed();
bool buttonLongPressed();
