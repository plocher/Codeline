// Appliance:SENSOR over MQTT: MqttSensorProxy (caller side, an ISensor) and
// MqttSensorAdapter (appliance side, wraps any ISensor). SENSOR is read-only,
// so the adapter subscribes to no /cmd. The proxy's restrictive fallback is
// active = true (occupied).
#pragma once

#include <string_view>

#include "codeline/appliance/ISensor.h"
#include "codeline/generated/SensorAppliance.h"
#include "codeline/mqtt/MqttAdapterBase.h"
#include "codeline/mqtt/MqttProxyBase.h"
#include "codeline/mqtt/SensorCodec.h"

namespace codeline::mqtt {

class MqttSensorProxy final : public MqttProxyBase<ISensor> {
public:
  MqttSensorProxy(IMqttClient& client, std::string_view appliance_topic)
      : MqttProxyBase(client, appliance_topic, kSensorAttributeTable, kSensorEvents) {}

  bool state() const override { return active_; }
  void setObserver(ISensorObserver* o) override { observer_ = o; }

private:
  ISensorObserver* observer_ = nullptr;
  bool active_ = true;

  // Every /state frame is reported, even if unchanged, so observers see the
  // same timeline as a local core.
  void report(bool active) {
    active_ = active;
    if (observer_) observer_->onStateChanged(active);
  }
  bool reportStateFrame(std::string_view payload) override {
    bool active = true;
    if (!decodeSensorState(payload, active)) return false;
    report(active);
    return true;
  }
  void fallBack() override { report(true); }
  void deliverEvent(const ApplianceEvent& ev) override {
    if (observer_) observer_->onEvent(ev);
  }
};

class MqttSensorAdapter final : public MqttAdapterBase, private ISensorObserver {
public:
  MqttSensorAdapter(IMqttClient& client, std::string_view channel, std::string_view id, ISensor& sensor)
      : MqttAdapterBase(client, channel, kSensorRole, id, sensor, kSensorAttributeTable, false), sensor_(sensor) {
    sensor_.setObserver(this);
  }

private:
  ISensor& sensor_;

  void onStateChanged(bool active) override {
    const Frame f = encodeSensorState(active);
    publishState(f.view(), f.ok());
  }
  void onEvent(const ApplianceEvent& ev) override { publishEvent(ev); }
  void onCommand(std::string_view) override {}
  void republishState() override { onStateChanged(sensor_.state()); }
};

}  // namespace codeline::mqtt
