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
};
#pragma pack(pop)

using ThresholdsWriteCb = void (*)(const Thresholds& t);
using CommandCb = void (*)(BleCommand cmd);

void bleInit(ThresholdsWriteCb onThresholds, CommandCb onCommand);
void bleNotifySensorPacket(const SensorPacket& packet);
void bleNotifyStatus(Urgency urgency, const uint8_t* reasons, uint8_t count);
void bleSetThresholdsValue(const Thresholds& t);
bool bleConnected();
SensorPacket makePacket(const SensorReadings& r, const DecisionResult& d);
