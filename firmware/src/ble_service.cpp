#include "ble_service.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <string.h>

namespace {
  BLEServer* server = nullptr;
  BLECharacteristic* chPacket = nullptr;
  BLECharacteristic* chStatus = nullptr;
  BLECharacteristic* chThresholds = nullptr;
  BLECharacteristic* chCommand = nullptr;
  bool deviceConnected = false;

  // THRESHOLDS wire layout (little-endian): f32 f32 f32 f32 u16 u16 f32 u16 f32. Mirrored in app.js.
  constexpr size_t THRESHOLDS_LEN = 30;

  ThresholdsWriteCb thresholdsCb = nullptr;
  CommandCb commandCb = nullptr;
  Thresholds currentThresholds = defaultThresholds();

  void encodeThresholds(uint8_t* buf, const Thresholds& t) {
    memcpy(buf + 0, &t.saturationSoonPct, 4);
    memcpy(buf + 4, &t.saturationNowPct, 4);
    memcpy(buf + 8, &t.edgeWetPct, 4);
    memcpy(buf + 12, &t.tempDiffRiseC, 4);
    memcpy(buf + 16, &t.tempSustainMin, 2);
    memcpy(buf + 18, &t.pHSustainMin, 2);
    memcpy(buf + 20, &t.pHAlert, 4);
    memcpy(buf + 24, &t.maxWearHours, 2);
    memcpy(buf + 26, &t.liftThreshold, 4);
  }

  bool decodeThresholds(const uint8_t* buf, size_t len, Thresholds& t) {
    if (len < THRESHOLDS_LEN) return false;
    memcpy(&t.saturationSoonPct, buf + 0, 4);
    memcpy(&t.saturationNowPct, buf + 4, 4);
    memcpy(&t.edgeWetPct, buf + 8, 4);
    memcpy(&t.tempDiffRiseC, buf + 12, 4);
    memcpy(&t.tempSustainMin, buf + 16, 2);
    memcpy(&t.pHSustainMin, buf + 18, 2);
    memcpy(&t.pHAlert, buf + 20, 4);
    memcpy(&t.maxWearHours, buf + 24, 2);
    memcpy(&t.liftThreshold, buf + 26, 4);
    return true;
  }

  class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer*) override { deviceConnected = true; }
    void onDisconnect(BLEServer* s) override {
      deviceConnected = false;
      s->startAdvertising();
    }
  };

  class ThresholdsCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
      auto v = c->getValue();
      if (v.length() == 0) return;
      Thresholds t = currentThresholds;
      if (decodeThresholds(reinterpret_cast<const uint8_t*>(v.c_str()), v.length(), t) &&
          thresholdsValid(t)) {
        currentThresholds = t;
        if (thresholdsCb) thresholdsCb(t);
        Serial.println("[ble] THRESHOLDS updated");
      } else {
        // The stack has already stored the rejected bytes; put the real values back.
        Serial.println("[ble] THRESHOLDS rejected (bad length or out of range)");
        uint8_t buf[THRESHOLDS_LEN];
        encodeThresholds(buf, currentThresholds);
        c->setValue(buf, sizeof(buf));
      }
    }
  };

  class CommandCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
      auto v = c->getValue();
      if (v.length() == 0) return;
      uint8_t cmd = static_cast<uint8_t>(v[0]);
      if (commandCb) commandCb(static_cast<BleCommand>(cmd));
      Serial.printf("[ble] COMMAND %u\n", cmd);
    }
  };
}  // namespace

void bleInit(ThresholdsWriteCb onThresholds, CommandCb onCommand) {
  thresholdsCb = onThresholds;
  commandCb = onCommand;

  BLEDevice::init("DressCheck");
  BLEDevice::setMTU(247);  // SENSOR_PACKET is 22 bytes; the default 23-byte MTU only carries 20
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* svc = server->createService(DRESSCHECK_SERVICE_UUID);

  chPacket = svc->createCharacteristic(
      DRESSCHECK_SENSOR_PACKET_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chPacket->addDescriptor(new BLE2902());

  chStatus = svc->createCharacteristic(
      DRESSCHECK_STATUS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chStatus->addDescriptor(new BLE2902());

  chThresholds = svc->createCharacteristic(
      DRESSCHECK_THRESHOLDS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  chThresholds->setCallbacks(new ThresholdsCallbacks());

  chCommand = svc->createCharacteristic(
      DRESSCHECK_COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
  chCommand->setCallbacks(new CommandCallbacks());

  uint8_t thBuf[THRESHOLDS_LEN];
  encodeThresholds(thBuf, currentThresholds);
  chThresholds->setValue(thBuf, sizeof(thBuf));

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(DRESSCHECK_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[ble] advertising DressCheck");
}

void bleSetThresholdsValue(const Thresholds& t) {
  currentThresholds = t;
  if (!chThresholds) return;
  uint8_t thBuf[THRESHOLDS_LEN];
  encodeThresholds(thBuf, t);
  chThresholds->setValue(thBuf, sizeof(thBuf));
}

void bleNotifySensorPacket(const SensorPacket& packet) {
  if (!chPacket || !deviceConnected) return;
  SensorPacket copy = packet;  // setValue() takes a non-const pointer
  chPacket->setValue(reinterpret_cast<uint8_t*>(&copy), sizeof(copy));
  chPacket->notify();
}

void bleNotifyStatus(Urgency urgency, const uint8_t* reasons, uint8_t count) {
  if (!chStatus || !deviceConnected) return;
  uint8_t buf[5] = {static_cast<uint8_t>(urgency), 0, 0, 0, 0};
  for (uint8_t i = 0; i < count && i < 4; i++) buf[1 + i] = reasons[i];
  chStatus->setValue(buf, sizeof(buf));
  chStatus->notify();
}

bool bleConnected() { return deviceConnected; }

SensorPacket makePacket(const SensorReadings& r, const DecisionResult& d) {
  SensorPacket p{};
  p.timestampMs = millis();
  p.centerPctX10 = static_cast<uint16_t>(r.centerPct * 10.0f);
  p.edgePctX10 = static_cast<uint16_t>(r.edgePct * 10.0f);
  p.tempDiffX100 = static_cast<int16_t>(r.tempDiffC * 100.0f);
  p.pHEstX100 = isnan(r.pHEst) ? 0 : static_cast<uint16_t>(r.pHEst * 100.0f);
  p.liftX1000 = static_cast<uint16_t>(r.lift * 1000.0f);
  p.hoursWornX10 = static_cast<uint16_t>(r.hoursWorn * 10.0f);
  p.faults = r.faults;
  p.urgency = static_cast<uint8_t>(d.urgency);
  p.reason0 = d.reasonCount > 0 ? d.reasons[0] : 0;
  p.reason1 = d.reasonCount > 1 ? d.reasons[1] : 0;
  p.reason2 = d.reasonCount > 2 ? d.reasons[2] : 0;
  p.reason3 = d.reasonCount > 3 ? d.reasons[3] : 0;
  return p;
}
