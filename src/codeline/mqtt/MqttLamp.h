// Appliance:LAMP over MQTT: MqttLampProxy (caller side, an ILamp) and
// MqttLampAdapter (appliance side, wraps any ILamp). /cmd and /state both
// carry ON | (ON). The proxy's fallback is on = false (not lit): a lamp that
// cannot be seen is never reported lit.
#pragma once

#include <string_view>

#include "codeline/appliance/ILamp.h"
#include "codeline/generated/LampAppliance.h"
#include "codeline/mqtt/LampCodec.h"
#include "codeline/mqtt/MqttAdapterBase.h"
#include "codeline/mqtt/MqttProxyBase.h"

namespace codeline::mqtt {

class MqttLampProxy final : public MqttProxyBase<ILamp> {
public:
  MqttLampProxy(IMqttClient& client, std::string_view appliance_topic)
      : MqttProxyBase(client, appliance_topic, kLampAttributeTable, kLampEvents) {}

  void setOn(bool on) override {
    const Frame f = encodeOnOff(on);
    sendCommand(f.view(), f.ok());
  }
  bool state() const override { return on_; }
  void setObserver(ILampObserver* o) override { observer_ = o; }

private:
  ILampObserver* observer_ = nullptr;
  bool on_ = false;

  // Every /state frame is reported, even if unchanged, so observers see the
  // same timeline as a local core.
  void report(bool on) {
    on_ = on;
    if (observer_) observer_->onStateChanged(on);
  }
  bool reportStateFrame(std::string_view payload) override {
    bool on = false;
    if (!decodeOnOff(payload, on)) return false;
    report(on);
    return true;
  }
  void fallBack() override { report(false); }
  void deliverEvent(const ApplianceEvent& ev) override {
    if (observer_) observer_->onEvent(ev);
  }
};

class MqttLampAdapter final : public MqttAdapterBase, private ILampObserver {
public:
  MqttLampAdapter(IMqttClient& client, std::string_view channel, std::string_view id, ILamp& lamp)
      : MqttAdapterBase(client, channel, kLampRole, id, lamp, kLampAttributeTable, true), lamp_(lamp) {
    lamp_.setObserver(this);
  }

private:
  ILamp& lamp_;

  void onStateChanged(bool on) override {
    const Frame f = encodeOnOff(on);
    publishState(f.view(), f.ok());
  }
  void onEvent(const ApplianceEvent& ev) override { publishEvent(ev); }
  void onCommand(std::string_view payload) override {
    bool on = false;
    if (decodeOnOff(payload, on)) lamp_.setOn(on);
  }
  void republishState() override { onStateChanged(lamp_.state()); }
};

}  // namespace codeline::mqtt
