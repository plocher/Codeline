// Caller side of Remote MQTT (docs/bindings/Cpp.md section 4, mode 3): an
// ITurnout whose commands become frames on {appliance}/cmd and /maint, and
// whose state and events come from {appliance}/state and /event.
//
// state() is the last /state frame that decoded cleanly. It falls to
// IN_TRANSIT (unproven) when:
//   - the appliance's will replaces /state (also raises UnavailableEvent),
//   - the proxy's own link to the broker breaks (also raises UnavailableEvent;
//     connect() again recovers from the retained /state),
//   - a /state frame is empty, malformed, or contradictory (also raises
//     UnavailableEvent, so health is unproven until attributes prove it).
// A retained /event frame is stale (/event is never retained) and dropped.
// After its own link breaks, tick() retries connect() on the
// ReconnectBackoff schedule until it succeeds; the retained /state then arrives fresh. Health stays not Ok until
// that state and a new AttributesEvent prove the appliance again.
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/appliance/ITurnout.h"
#include "codeline/generated/TurnoutAppliance.h"
#include "codeline/mqtt/IMqttClient.h"
#include "codeline/mqtt/ReconnectBackoff.h"
#include "codeline/mqtt/TurnoutCodec.h"

namespace codeline::mqtt {

class MqttTurnoutProxy final : public ITurnout, private IMqttHandler, private IMqttLinkObserver {
public:
  using Topic = FixedString<127>;

  // appliance_topic is "{channel}/turnout/{id}".
  MqttTurnoutProxy(IMqttClient& client, std::string_view appliance_topic) : client_(client) {
    cmd_.append(appliance_topic).append("/").append(kTopicCmd);
    state_.append(appliance_topic).append("/").append(kTopicState);
    maint_.append(appliance_topic).append("/").append(kTopicMaint);
    event_.append(appliance_topic).append("/").append(kTopicEvent);
    will_.append(appliance_topic).append("/").append(kWillTopic);
  }

  void connect(std::string_view client_id) {
    if (client_id.data() != id_.view().data()) {
      id_ = Topic();
      id_.append(client_id);
    }
    client_.setLinkObserver(this);
    client_.connect(id_.view(), nullptr);
    if (!client_.connected()) {
      backoff_.setKey(id_.view());
      if (backoff_.pending()) backoff_.failed();
      else backoff_.lost();
      return;
    }
    backoff_.connected();
    client_.subscribe(state_.view(), this);  // the retained state arrives now
    client_.subscribe(event_.view(), this);
  }

  // ---- ITurnout ----

  void setAlignment(TurnoutCommand cmd) override { client_.publish(cmd_.view(), encodeCommand(cmd), kTopicCmdRetained); }
  TurnoutState state() const override { return state_value_; }
  void setObserver(ITurnoutObserver* o) override { observer_ = o; }
  // Nothing else is timed on the caller side; tick() only reconnects after a
  // link loss, on the ReconnectBackoff schedule.
  void tick(uint32_t now_ms) override {
    backoff_.tick(now_ms);
    if (backoff_.due()) connect(id_.view());
  }

  // ---- IMaintenance ----

  void identify(IdentifyMode mode, uint32_t duration_ms) override {
    Frame f;
    f.append(verb(MaintenanceCommand::Identify)).append(" ");
    if (mode == IdentifyMode::Pulse) f.append(duration_ms);
    else f.append(nameOf(mode));
    send(f);
  }
  void exportAttributes() override { sendVerb(MaintenanceCommand::ExportAttributes); }
  void exportConfig() override { sendVerb(MaintenanceCommand::ExportConfig); }
  void offline() override { sendVerb(MaintenanceCommand::Offline); }
  void online() override { sendVerb(MaintenanceCommand::Online); }
  void clearFault() override { sendVerb(MaintenanceCommand::ClearFault); }

  void setAttribute(std::string_view path, SetOp op, AttrValue value) override {
    const AttributeInfo* a = kTurnoutAttributeTable.find(path);
    if (!a || !isValid(*a, value)) return;  // not encodable: dropped at the codec
    Frame f;
    f.append(verb(MaintenanceCommand::SetAttribute)).append(" ").append(a->path).append(" ");
    f.append(kSetOpTokens[static_cast<uint8_t>(op)]).append(" ");
    appendValue(f, *a, value);
    send(f);
  }

  void clearAttribute(std::string_view path) override {
    Frame f;
    f.append(verb(MaintenanceCommand::ClearAttribute)).append(" ").append(path);
    send(f);
  }

  // A map the core would reject (unknown, not NV config, out of range) is
  // never spelled: it goes as an empty IMPORT, which the core rejects the
  // same way, changing nothing and answering CONFIG.
  void importConfig(const AttrMap& config) override {
    Frame f;
    f.append(verb(MaintenanceCommand::ImportConfig));
    if (importable(kTurnoutAttributeTable, config) && config.count > 0) {
      f.append(" ");
      appendMap(f, config);
    }
    send(f);
  }

  void cycle(uint16_t count, uint32_t dwell_ms) override {
    Frame f;
    f.append(verb(MaintenanceCommand::Cycle)).append(" ").append(count).append(" ").append(dwell_ms);
    send(f);
  }

private:
  IMqttClient& client_;
  ITurnoutObserver* observer_ = nullptr;
  TurnoutState state_value_ = TurnoutState::InTransit;
  Topic cmd_, state_, maint_, event_, will_;
  Topic id_;                    // client id, for reconnecting
  ReconnectBackoff backoff_;    // this proxy's own broker connection

  void send(const Frame& f) {
    if (f.ok()) client_.publish(maint_.view(), f.view(), kTopicMaintRetained);
  }
  void sendVerb(MaintenanceCommand c) { client_.publish(maint_.view(), verb(c), kTopicMaintRetained); }

  void onMessage(std::string_view topic, std::string_view payload, bool retained) override {
    if (topic == will_.view() && payload == kWillPayload) {
      becomeUnavailable();
    } else if (topic == state_.view()) {
      onState(payload);
    } else if (topic == event_.view()) {
      if (retained && !kTopicEventRetained) return;
      ApplianceEvent ev;
      AttrMap map;
      if (decodeEvent(payload, kTurnoutAttributeTable, kTurnoutEvents, ev, map) && observer_) observer_->onEvent(ev);
    }
  }

  void onConnectionLost() override {
    backoff_.setKey(id_.view());
    backoff_.lost();
    becomeUnavailable();
  }

  void becomeUnavailable() {
    report(TurnoutState::InTransit);
    if (observer_) observer_->onEvent(ApplianceEvent::of(EventKind::Unavailable));
  }

  // A frame that does not decode proves nothing about the points.
  void onState(std::string_view payload) {
    TurnoutState s = TurnoutState::InTransit;
    if (decodeState(payload, s)) report(s);
    else becomeUnavailable();
  }

  // Every /state frame is reported, even if unchanged, so observers see the
  // same timeline as a local core (which re-announces state on restart).
  void report(TurnoutState s) {
    state_value_ = s;
    if (observer_) observer_->onStateChanged(s);
  }
};

}  // namespace codeline::mqtt
