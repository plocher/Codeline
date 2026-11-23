// Caller side of Remote MQTT (docs/bindings/Cpp.md section 4, mode 3), the
// part every appliance shares: topics, Cluster:MAINTENANCE as /maint frames,
// /event decoding, and loss of the appliance. A derived proxy supplies the
// typed API: its commands (sendCommand), its /state decoding, and the
// restrictive state it falls back to.
//
// state() falls back to the derived proxy's restrictive value when:
//   - the appliance's will replaces /state (also raises UnavailableEvent),
//   - the proxy's own link to the broker breaks (also raises UnavailableEvent;
//     connect() again recovers from the retained /state),
//   - a /state frame is empty, malformed, or contradictory (also raises
//     UnavailableEvent: the binding can no longer see the appliance, so
//     health stays unproven until attributes prove it again).
// The fallback state is reported before the UnavailableEvent, as
// ApplianceHealth expects.
//
// After its own link breaks, tick() retries connect() on the
// ReconnectBackoff schedule until it succeeds; the retained /state then arrives fresh. Health stays not Ok until
// that state and a new AttributesEvent prove the appliance again.
//
// Every /event frame that arrives retained is dropped: /event is never
// retained (MQTT.md section 4), so a retained one is stale and must not
// prove health. Events the appliance cannot emit (a BTN<k> on a mast) are
// dropped too. (MqttTurnoutProxy predates this base.)
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/core/Attributes.h"
#include "codeline/mqtt/ApplianceCodec.h"
#include "codeline/mqtt/IMqttClient.h"
#include "codeline/mqtt/ReconnectBackoff.h"

namespace codeline::mqtt {

template <class Interface>
class MqttProxyBase : public Interface, private IMqttHandler, private IMqttLinkObserver {
public:
  using Topic = FixedString<127>;

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
    sendMaint(f.view(), f.ok());
  }
  void exportAttributes() override { sendVerb(MaintenanceCommand::ExportAttributes); }
  void exportConfig() override { sendVerb(MaintenanceCommand::ExportConfig); }
  void offline() override { sendVerb(MaintenanceCommand::Offline); }
  void online() override { sendVerb(MaintenanceCommand::Online); }
  void clearFault() override { sendVerb(MaintenanceCommand::ClearFault); }

  void setAttribute(std::string_view path, SetOp op, AttrValue value) override {
    const AttributeInfo* a = table_.find(path);
    if (!a || !isValid(*a, value)) return;  // not encodable: dropped at the codec
    Frame f;
    f.append(verb(MaintenanceCommand::SetAttribute)).append(" ").append(a->path).append(" ");
    f.append(kSetOpTokens[static_cast<uint8_t>(op)]).append(" ");
    appendValue(f, *a, value);
    sendMaint(f.view(), f.ok());
  }

  void clearAttribute(std::string_view path) override {
    Frame f;
    f.append(verb(MaintenanceCommand::ClearAttribute)).append(" ").append(path);
    sendMaint(f.view(), f.ok());
  }

  // A map the core would reject (unknown, not NV config, out of range) is
  // never spelled: it goes as an empty IMPORT, which the core rejects the
  // same way, changing nothing and answering CONFIG.
  void importConfig(const AttrMap& config) override {
    Frame f;
    f.append(verb(MaintenanceCommand::ImportConfig));
    if (importable(table_, config) && config.count > 0) {
      f.append(" ");
      appendMap(f, config);
    }
    sendMaint(f.view(), f.ok());
  }

  void cycle(uint16_t count, uint32_t dwell_ms) override {
    Frame f;
    f.append(verb(MaintenanceCommand::Cycle)).append(" ").append(count).append(" ").append(dwell_ms);
    sendMaint(f.view(), f.ok());
  }

protected:
  // appliance_topic is "{channel}/{role}/{id}".
  // `events` is the generated k<Name>Events: which EventKinds this appliance emits.
  MqttProxyBase(IMqttClient& client, std::string_view appliance_topic, const AttributeTable& table,
                const bool* events)
      : client_(client), table_(table), events_(events) {
    cmd_.append(appliance_topic).append("/").append(kTopicCmd);
    state_.append(appliance_topic).append("/").append(kTopicState);
    maint_.append(appliance_topic).append("/").append(kTopicMaint);
    event_.append(appliance_topic).append("/").append(kTopicEvent);
    will_.append(appliance_topic).append("/").append(kWillTopic);
  }

  void sendCommand(std::string_view payload, bool ok) {
    if (ok) client_.publish(cmd_.view(), payload, kTopicCmdRetained);
  }

  // Decode a /state frame and report it; false (nothing reported) if it does not decode.
  virtual bool reportStateFrame(std::string_view payload) = 0;
  // Report the restrictive state (unproven).
  virtual void fallBack() = 0;
  virtual void deliverEvent(const ApplianceEvent& ev) = 0;

private:
  IMqttClient& client_;
  const AttributeTable& table_;
  const bool* events_;
  Topic id_;                    // client id, for reconnecting
  ReconnectBackoff backoff_;    // this proxy's own broker connection
  Topic cmd_, state_, maint_, event_, will_;

  void sendMaint(std::string_view payload, bool ok) {
    if (ok) client_.publish(maint_.view(), payload, kTopicMaintRetained);
  }
  void sendVerb(MaintenanceCommand c) { client_.publish(maint_.view(), verb(c), kTopicMaintRetained); }

  void onMessage(std::string_view topic, std::string_view payload, bool retained) override {
    if (topic == will_.view() && payload == kWillPayload) {
      becomeUnavailable();
    } else if (topic == state_.view()) {
      if (!reportStateFrame(payload)) becomeUnavailable();
    } else if (topic == event_.view()) {
      if (retained && !kTopicEventRetained) return;
      ApplianceEvent ev;
      AttrMap map;
      if (decodeEvent(payload, table_, events_, ev, map)) deliverEvent(ev);
    }
  }

  void onConnectionLost() override {
    backoff_.setKey(id_.view());
    backoff_.lost();
    becomeUnavailable();
  }

  void becomeUnavailable() {
    fallBack();
    deliverEvent(ApplianceEvent::of(EventKind::Unavailable));
  }
};

}  // namespace codeline::mqtt
