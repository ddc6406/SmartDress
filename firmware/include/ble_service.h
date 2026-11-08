#pragma once

#include <Arduino.h>
#include "decision.h"
#include "sensors.h"

// Custom 128-bit UUIDs (DressCheck prototype service)
#define DRESSCHECK_SERVICE_UUID        "dcc00001-a1b2-c3d4-e5f6-000000000001"
#define DRESSCHECK_SENSOR_PACKET_UUID  "dcc00001-a1b2-c3d4-e5f6-000000000002"
#define DRESSCHECK_STATUS_UUID         "dcc00001-a1b2-c3d4-e5f6-000000000003"
#define DRESSCHECK_THRESHOLDS_UUID     "dcc00001-a1b2-c3d4-e5f6-000000000004"
#define DRESSCHECK_COMMAND_UUID        "dcc00001-a1b2-c3d4-e5f6-000000000005"

enum BleCommand : uint8_t {
  CMD_NONE = 0,
  CMD_NEW_DRESSING = 1,
  CMD_ACK_ALERT = 2,
  CMD_SELF_TEST = 3,
  CMD_CALIBRATE_WHITE = 4,  // capture the colour sensor's white reference
  CMD_BUZZ_ON = 5,          // manual buzz: motor on (5 s safety cut-off) until CMD_BUZZ_OFF
  CMD_BUZZ_OFF = 6,
};

// Packed SENSOR_PACKET (little-endian on wire)
#pragma pack(push, 1)
struct SensorPacket {
  uint32_t timestampMs;
  uint16_t centerPctX10;   // percent * 10
  uint16_t edgePctX10;
  int16_t tempDiffX100;    // °C * 100
  uint16_t pHEstX100;      // pH * 100
  uint16_t liftX1000;      // 0..1 * 1000
  uint16_t hoursWornX10;   // hours * 10
  uint8_t faults;
  uint8_t urgency;         // Urgency enum
  uint8_t reason0;
  uint8_t reason1;
  uint8_t reason2;
  uint8_t reason3;
  int16_t tWoundX100;      // wound thermistor °C * 100; TEMP_UNAVAILABLE if open/shorted
  int16_t tRefX100;        // reference thermistor °C * 100
  uint16_t centerMv;       // raw pin voltages, exactly what each sensor pin sees
  uint16_t edgeMv;
  uint16_t liftMv;
  uint16_t tWoundMv;
  uint16_t tRefMv;
  uint16_t colR;           // raw colour-sensor counts (LED on)
  uint16_t colG;
  uint16_t colB;
  uint16_t colC;
  uint16_t matchX10;       // colour points off the chart path * 10 (MATCH_NO_READING if none)
  uint16_t nearestPhX100;  // best-guess pH * 100, always sent when there is a reading (0xFFFF if none)
  uint16_t colFlags;       // bit 0: white reference captured
};

constexpr int16_t TEMP_UNAVAILABLE = INT16_MIN;
constexpr uint16_t VALUE_UNAVAILABLE = 0xFFFF;  // centre/edge %, lift, hours worn before a session
constexpr uint16_t MATCH_NO_READING = 0xFFFF;  // no sensor, nothing under it, or glare
constexpr uint16_t COLFLAG_WHITE_SET = 1 << 0;
static_assert(sizeof(SensorPacket) == 50, "SENSOR_PACKET layout is mirrored in app/app.js");
#pragma pack(pop)

using ThresholdsWriteCb = void (*)(const Thresholds& t);
using CommandCb = void (*)(BleCommand cmd);

void bleInit(ThresholdsWriteCb onThresholds, CommandCb onCommand);
void bleNotifySensorPacket(const SensorPacket& packet);
void bleNotifyStatus(Urgency urgency, const uint8_t* reasons, uint8_t count);
void bleSetThresholdsValue(const Thresholds& t);
bool bleConnected();
SensorPacket makePacket(const SensorReadings& r, const DecisionResult& d);
