// Appliance:SIGNALMAST over MQTT: MqttMastProxy (caller side, an IMast) and
// MqttMastAdapter (appliance side, wraps any IMast). The proxy's restrictive
// fallback is { STOP, lit = true }, i.e. MastState{}.
#pragma once

#include <string_view>

#include "codeline/appliance/ISignalMast.h"
#include "codeline/generated/SignalmastAppliance.h"
#include "codeline/mqtt/MastCodec.h"
#include "codeline/mqtt/MqttAdapterBase.h"
#include "codeline/mqtt/MqttProxyBase.h"

namespace codeline::mqtt {

class MqttMastProxy final : public MqttProxyBase<IMast> {
public:
  MqttMastProxy(IMqttClient& client, std::string_view appliance_topic)
      : MqttProxyBase(client, appliance_topic, kSignalmastAttributeTable, kSignalmastEvents) {}

  void setIndication(SignalIndication indication, bool lit) override {
    const Frame f = encodeMastCommand(indication, lit);
    sendCommand(f.view(), f.ok());
  }
  void setLit(bool lit) override {
    const Frame f = encodeLitCommand(lit);
    sendCommand(f.view(), f.ok());
  }
  MastState state() const override { return state_; }
  void setObserver(IMastObserver* o) override { observer_ = o; }

private:
  IMastObserver* observer_ = nullptr;
  MastState state_;

  void report(MastState s) {
    state_ = s;
    if (observer_) observer_->onStateChanged(s);
  }
  bool reportStateFrame(std::string_view payload) override {
    MastState s;
    if (!decodeMastState(payload, s)) return false;
    report(s);
    return true;
  }
  void fallBack() override { report(MastState{}); }
  void deliverEvent(const ApplianceEvent& ev) override {
    if (observer_) observer_->onEvent(ev);
  }
};

class MqttMastAdapter final : public MqttAdapterBase, private IMastObserver {
public:
  MqttMastAdapter(IMqttClient& client, std::string_view channel, std::string_view id, IMast& mast)
      : MqttAdapterBase(client, channel, kSignalmastRole, id, mast, kSignalmastAttributeTable, true), mast_(mast) {
    mast_.setObserver(this);
  }

private:
  IMast& mast_;

  void onStateChanged(MastState s) override {
    const Frame f = encodeMastState(s);
    publishState(f.view(), f.ok());
  }
  void onEvent(const ApplianceEvent& ev) override { publishEvent(ev); }
  void onCommand(std::string_view payload) override {
    MastCommand c;
    if (!decodeMastCommand(payload, c)) return;
    if (c.has_indication) mast_.setIndication(c.indication, c.lit);
    else mast_.setLit(c.lit);
  }
  void republishState() override { onStateChanged(mast_.state()); }
};

}  // namespace codeline::mqtt
