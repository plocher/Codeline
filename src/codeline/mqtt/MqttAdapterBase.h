// Appliance side of Remote MQTT (docs/bindings/Cpp.md section 4, mode 3), the
// part every appliance shares. It projects a core onto
//   {appliance}/cmd    subscribed, unless the appliance has no commands (SENSOR)
//   {appliance}/maint  subscribed
//   {channel}/all      subscribed, curated broadcast only
//   {appliance}/state  published, with the last will
//   {appliance}/event  published
// A message arriving retained on a topic that is never retained (cmd, maint,
// all) is dropped: a stale command must never actuate a waking appliance
// (MQTT.md section 4.1). A derived adapter decodes /cmd and encodes /state.
//
// The adapter does not tick the core; its own tick() runs the jitter before
// answering a broadcast EXPORT CONFIG, and reconnection.//
// Broker outage (owner ruling): losing the link does nothing to the core or
// its outputs. The adapter observes its own link; after a loss, tick()
// retries connect() on the ReconnectBackoff schedule (about 1 s, then
// doubling to a 16 s cap, skewed per client id) until it succeeds. A reconnect re-registers the will,
// re-subscribes (a clean session: nothing queued from before is delivered,
// and anything retained on cmd/maint/all is dropped), and republishes the
// current /state retained, since the broker may hold the will's UNAVAILABLE,
// a stale state, or nothing at all.
// (MqttTurnoutAdapter predates this base and does the same.)
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/appliance/IMaintenance.h"
#include "codeline/core/TickClock.h"
#include "codeline/mqtt/ApplianceCodec.h"
#include "codeline/mqtt/IMqttClient.h"
#include "codeline/mqtt/ReconnectBackoff.h"

namespace codeline::mqtt {

class MqttAdapterBase : private IMqttHandler, private IMqttLinkObserver {
public:
  using Topic = FixedString<127>;

  std::string_view applianceTopic() const { return base_.view(); }

  // (Re)connects with the last will and subscribes. After a reconnect the
  // current state is published again, since a will may have replaced it.
  // Until the core has reported a state (a mast reports only once its heads
  // prove STOP), /state is set to the will payload instead, so no stale
  // retained state from an earlier session reads as this boot's state.
  void connect() {
    const MqttWill will{will_.view(), kWillPayload, kWillRetained};
    client_.setLinkObserver(this);
    client_.connect(base_.view(), &will);
    if (!client_.connected()) {
      backoff_.setKey(base_.view());
      if (backoff_.pending()) backoff_.failed();
      else backoff_.lost();
      return;
    }
    backoff_.connected();
    if (has_cmd_) client_.subscribe(cmd_.view(), this);
    client_.subscribe(maint_.view(), this);
    client_.subscribe(all_.view(), this);
    if (announced_) republishState();
    else client_.publish(state_.view(), kWillPayload, kWillRetained);
  }

  void tick(uint32_t now_ms) {
    clock_.advanceTo(now_ms);
    backoff_.tick(now_ms);
    if (backoff_.due()) connect();
    if (export_pending_ && export_timer_ >= export_jitter_ms_) {
      export_pending_ = false;
      target_.exportConfig();
    }
  }

protected:
  MqttAdapterBase(IMqttClient& client, std::string_view channel, std::string_view role, std::string_view id,
                  IMaintenance& target, const AttributeTable& table, bool has_cmd)
      : client_(client), target_(target), table_(table), has_cmd_(has_cmd) {
    base_.append(channel).append("/").append(role).append("/").append(id);
    cmd_.append(base_.view()).append("/").append(kTopicCmd);
    state_.append(base_.view()).append("/").append(kTopicState);
    maint_.append(base_.view()).append("/").append(kTopicMaint);
    event_.append(base_.view()).append("/").append(kTopicEvent);
    will_.append(base_.view()).append("/").append(kWillTopic);
    all_.append(channel).append("/").append(kTopicAll);
    for (char c : base_.view()) rng_ = rng_ * 31u + static_cast<uint8_t>(c);
  }

  // Called by the derived adapter's observer.
  void publishState(std::string_view frame, bool ok) {
    announced_ = true;
    if (ok) client_.publish(state_.view(), frame, kTopicStateRetained);
  }
  void publishEvent(const ApplianceEvent& ev) {
    Frame f;
    if (encodeEvent(ev, f)) client_.publish(event_.view(), f.view(), kTopicEventRetained);
  }

  virtual void onCommand(std::string_view payload) = 0;
  virtual void republishState() = 0;

private:
  IMqttClient& client_;
  IMaintenance& target_;
  const AttributeTable& table_;
  bool has_cmd_;
  Topic base_, cmd_, state_, maint_, event_, will_, all_;
  bool announced_ = false;
  ReconnectBackoff backoff_;  // this adapter's own broker connection

  TickClock clock_;
  ElapsedMillis export_timer_{clock_};  // read only while export_pending_
  bool export_pending_ = false;
  uint32_t export_jitter_ms_ = 0;
  uint32_t rng_ = 2166136261u;

  void onMessage(std::string_view topic, std::string_view payload, bool retained) override {
    if (has_cmd_ && topic == cmd_.view()) {
      if (retained && !kTopicCmdRetained) return;
      onCommand(payload);
    } else if (topic == maint_.view()) {
      if (retained && !kTopicMaintRetained) return;
      MaintFrame f;
      if (decodeMaint(payload, table_, f)) applyMaint(f, target_);
    } else if (topic == all_.view()) {
      if (retained && !kTopicAllRetained) return;
      onBroadcast(payload);
    }
  }

  void onConnectionLost() override {
    backoff_.setKey(base_.view());
    backoff_.lost();
  }

  void onBroadcast(std::string_view payload) {
    // Cluster:GROUPS is not composed, so this appliance is in no group.
    Tokens t(payload);
    std::string_view first;
    if (t.next(first) && first == kGroupPrefix) return;
    MaintFrame f;
    if (!decodeMaint(payload, table_, f)) return;
    if (!kMaintenanceBroadcast[static_cast<uint8_t>(f.command)]) return;
    if (f.command == MaintenanceCommand::ExportConfig) {
      rng_ = rng_ * 1664525u + 1013904223u;
      export_jitter_ms_ = (rng_ >> 8) % (kBroadcastJitterMaxMs + 1);
      export_timer_ = 0;
      export_pending_ = true;
      return;
    }
    applyMaint(f, target_);
  }
};

}  // namespace codeline::mqtt
