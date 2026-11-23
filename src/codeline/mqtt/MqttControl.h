// Appliance:CONTROL over MQTT: MqttControlProxy (caller side, an IControl)
// and MqttControlAdapter (appliance side, wraps any IControl). ButtonEvent
// travels on /event as BTN<k> / (BTN<k>). The proxy's restrictive fallback is
// the empty frame (count 0), which proves no lever position.
#pragma once

#include <string_view>

#include "codeline/appliance/IControl.h"
#include "codeline/generated/ControlAppliance.h"
#include "codeline/mqtt/ControlCodec.h"
#include "codeline/mqtt/MqttAdapterBase.h"
#include "codeline/mqtt/MqttProxyBase.h"

namespace codeline::mqtt {

class MqttControlProxy final : public MqttProxyBase<IControl> {
public:
  MqttControlProxy(IMqttClient& client, std::string_view appliance_topic)
      : MqttProxyBase(client, appliance_topic, kControlAttributeTable, kControlEvents) {}

  void setLamps(BitVector lamps) override {
    if (lamps.count > BitVector::kMaxCount) return;
    const VectorFrame f = encodeLamps(lamps);
    sendCommand(f.view(), f.ok());
  }
  BitVector state() const override { return switches_; }
  void setObserver(IControlObserver* o) override { observer_ = o; }

private:
  IControlObserver* observer_ = nullptr;
  BitVector switches_;

  void report(BitVector v) {
    switches_ = v;
    if (observer_) observer_->onStateChanged(v);
  }
  bool reportStateFrame(std::string_view payload) override {
    BitVector v;
    if (!decodeSwitches(payload, v)) return false;
    report(v);
    return true;
  }
  void fallBack() override { report(BitVector{}); }
  void deliverEvent(const ApplianceEvent& ev) override {
    if (observer_) observer_->onEvent(ev);
  }
};

class MqttControlAdapter final : public MqttAdapterBase, private IControlObserver {
public:
  MqttControlAdapter(IMqttClient& client, std::string_view channel, std::string_view id, IControl& control)
      : MqttAdapterBase(client, channel, kControlRole, id, control, kControlAttributeTable, true), control_(control) {
    control_.setObserver(this);
  }

private:
  IControl& control_;

  void onStateChanged(BitVector switches) override {
    const VectorFrame f = encodeSwitches(switches);
    publishState(f.view(), f.ok() && switches.count > 0);
  }
  void onEvent(const ApplianceEvent& ev) override { publishEvent(ev); }
  void onCommand(std::string_view payload) override {
    BitVector lamps;
    if (decodeLamps(payload, lamps)) control_.setLamps(lamps);
  }
  void republishState() override { onStateChanged(control_.state()); }
};

}  // namespace codeline::mqtt
