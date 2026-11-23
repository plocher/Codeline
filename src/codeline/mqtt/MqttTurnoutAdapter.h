// Appliance side of Remote MQTT (docs/bindings/Cpp.md section 4, mode 3):
// wraps any ITurnout, takes its observer slot, and projects it onto
//   {appliance}/cmd    subscribed
//   {appliance}/maint  subscribed
//   {channel}/all      subscribed, curated broadcast only
//   {appliance}/state  published, with the last will
//   {appliance}/event  published
// Retention of each topic comes from the schema. A message arriving retained
// on a topic that is never retained (cmd, maint, all) is dropped: a stale
// command must never actuate a waking appliance (MQTT.md section 4.1).
//
// The adapter does not tick the wrapped turnout; its own tick() runs the
// jitter before answering a broadcast EXPORT CONFIG, and reconnection.//
// Broker outage (owner ruling): losing the link does nothing to the core or
// its outputs. The adapter observes its own link; after a loss, tick()
// retries connect() on the ReconnectBackoff schedule (about 1 s, then
// doubling to a 16 s cap, skewed per client id) until it succeeds. A reconnect re-registers the will,
// re-subscribes (a clean session: nothing queued from before is delivered,
// and anything retained on cmd/maint/all is dropped), and republishes the
// current /state retained, since the broker may hold the will's UNAVAILABLE,
// a stale state, or nothing at all.
// Until the turnout has reported a state, /state is set to the will payload,
// so no stale retained state from an earlier session reads as this boot's.
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/appliance/ITurnout.h"
#include "codeline/core/TickClock.h"
#include "codeline/generated/TurnoutAppliance.h"
#include "codeline/mqtt/IMqttClient.h"
#include "codeline/mqtt/ReconnectBackoff.h"
#include "codeline/mqtt/TurnoutCodec.h"

namespace codeline::mqtt {

class MqttTurnoutAdapter final : private ITurnoutObserver, private IMqttHandler, private IMqttLinkObserver {
public:
  using Topic = FixedString<127>;

  MqttTurnoutAdapter(IMqttClient& client, std::string_view channel, std::string_view id, ITurnout& turnout)
      : client_(client), turnout_(turnout) {
    base_.append(channel).append("/").append(kTurnoutRole).append("/").append(id);
    cmd_.append(base_.view()).append("/").append(kTopicCmd);
    state_.append(base_.view()).append("/").append(kTopicState);
    maint_.append(base_.view()).append("/").append(kTopicMaint);
    event_.append(base_.view()).append("/").append(kTopicEvent);
    will_.append(base_.view()).append("/").append(kWillTopic);
    all_.append(channel).append("/").append(kTopicAll);
    for (char c : base_.view()) rng_ = rng_ * 31u + static_cast<uint8_t>(c);
    turnout_.setObserver(this);
  }

  std::string_view applianceTopic() const { return base_.view(); }

  // (Re)connects with the last will and subscribes. After a reconnect the
  // current state is published again, since a will may have replaced it.
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
    client_.subscribe(cmd_.view(), this);
    client_.subscribe(maint_.view(), this);
    client_.subscribe(all_.view(), this);
    if (announced_) publishState(turnout_.state());
    else client_.publish(state_.view(), kWillPayload, kWillRetained);
  }

  void tick(uint32_t now_ms) {
    clock_.advanceTo(now_ms);
    backoff_.tick(now_ms);
    if (backoff_.due()) connect();
    if (export_pending_ && export_timer_ >= export_jitter_ms_) {
      export_pending_ = false;
      turnout_.exportConfig();
    }
  }

private:
  IMqttClient& client_;
  ITurnout& turnout_;
  Topic base_, cmd_, state_, maint_, event_, will_, all_;
  bool announced_ = false;
  ReconnectBackoff backoff_;  // this adapter's own broker connection

  TickClock clock_;
  ElapsedMillis export_timer_{clock_};  // read only while export_pending_
  bool export_pending_ = false;
  uint32_t export_jitter_ms_ = 0;
  uint32_t rng_ = 2166136261u;

  // ---- appliance -> wire ----

  void publishState(TurnoutState s) { client_.publish(state_.view(), encodeState(s), kTopicStateRetained); }

  void onStateChanged(TurnoutState s) override {
    announced_ = true;
    publishState(s);
  }

  void onEvent(const ApplianceEvent& ev) override {
    Frame f;
    if (encodeEvent(ev, f)) client_.publish(event_.view(), f.view(), kTopicEventRetained);
  }

  void onConnectionLost() override {
    backoff_.setKey(base_.view());
    backoff_.lost();
  }

  // ---- wire -> appliance ----

  void onMessage(std::string_view topic, std::string_view payload, bool retained) override {
    if (topic == cmd_.view()) {
      if (retained && !kTopicCmdRetained) return;
      TurnoutCommand c;
      if (decodeCommand(payload, c)) turnout_.setAlignment(c);  // NORMAL REVERSE is dropped here
    } else if (topic == maint_.view()) {
      if (retained && !kTopicMaintRetained) return;
      MaintFrame f;
      if (decodeMaint(payload, kTurnoutAttributeTable, f)) applyMaint(f, turnout_);
    } else if (topic == all_.view()) {
      if (retained && !kTopicAllRetained) return;
      onBroadcast(payload);
    }
  }

  void onBroadcast(std::string_view payload) {
    // Cluster:GROUPS is not composed, so this appliance is in no group.
    Tokens t(payload);
    std::string_view first;
    if (t.next(first) && first == kGroupPrefix) return;
    MaintFrame f;
    if (!decodeMaint(payload, kTurnoutAttributeTable, f)) return;
    if (!kMaintenanceBroadcast[static_cast<uint8_t>(f.command)]) return;
    if (f.command == MaintenanceCommand::ExportConfig) {
      rng_ = rng_ * 1664525u + 1013904223u;
      export_jitter_ms_ = (rng_ >> 8) % (kBroadcastJitterMaxMs + 1);
      export_timer_ = 0;
      export_pending_ = true;
      return;
    }
    applyMaint(f, turnout_);
  }
};

}  // namespace codeline::mqtt
