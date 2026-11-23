// When one broker connection retries connect() after losing its link (owner
// ruling): never on every tick, which would flood the Wi-Fi channel.
//
// Policy (the numbers are schema/bindings/mqtt.json "reconnect", generated
// into MqttTables.h):
//   base_0     = kReconnectInitialMs                          (1 s)
//   base_k+1   = min(base_k * kReconnectFactor, kReconnectMaxMs)   (1, 2, 4, 8, 16, 16, ... s)
//   skew       = fnv1a32(connection key) % (kReconnectSkewPermille + 1)   (0..250 per mille)
//   wait_k     = base_k + base_k * skew / 1000                (integer ms)
// Attempt k happens at the first tick at least wait_k after the link loss
// (k = 0) or after attempt k-1 failed. So retries settle at once every
// 16..20 s. A successful connect resets the sequence; a deliberate
// disconnect() never starts one (only a link loss does).
//
// The skew is deterministic (no random source, no wall clock): connections
// that lost the broker in the same instant retry at different instants
// because their keys differ, and scenarios stay reproducible.
//
// One instance belongs to one broker connection, not to one appliance: the
// key is the connection's client id. Today every adapter and proxy owns its
// own IMqttClient, so each owns one of these. A host that shares one
// connection among several appliances would own a single instance, so N
// appliances on it would produce one retry sequence, not N.
//
// Time comes only from tick(now_ms) (rollover-safe ElapsedMillis). After a
// long idle or a clock wrap, at most one attempt is due at the next tick;
// the next wait is counted from that attempt, so there is never a burst.
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/core/TickClock.h"
#include "codeline/generated/MqttTables.h"

namespace codeline::mqtt {

class ReconnectBackoff {
public:
  // The key is the connection's client id; set it before the first lost().
  void setKey(std::string_view connection_key) { skew_permille_ = skewPermille(connection_key); }

  static uint32_t skewPermille(std::string_view key) {
    uint32_t h = 2166136261u;  // FNV-1a, 32 bit
    for (char c : key) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return h % (kReconnectSkewPermille + 1);
  }

  void tick(uint32_t now_ms) { clock_.advanceTo(now_ms); }

  // The link broke (not a deliberate disconnect), or the first connect
  // failed: the sequence starts from the beginning.
  void lost() {
    pending_ = true;
    base_ms_ = kReconnectInitialMs;
    timer_ = 0;
  }
  // True when the owner should call connect() now (after tick()).
  bool due() const { return pending_ && timer_ >= waitMs(); }
  // The attempt failed: wait longer.
  void failed() {
    base_ms_ = base_ms_ >= kReconnectMaxMs / kReconnectFactor ? kReconnectMaxMs : base_ms_ * kReconnectFactor;
    timer_ = 0;
  }
  // Connected: the sequence ends (lost() starts the next one from the beginning).
  void connected() { pending_ = false; }

  bool pending() const { return pending_; }
  uint32_t waitMs() const { return base_ms_ + base_ms_ * skew_permille_ / 1000u; }

private:
  TickClock clock_;
  ElapsedMillis timer_{clock_};  // read only while pending_
  uint32_t skew_permille_ = 0;
  uint32_t base_ms_ = kReconnectInitialMs;
  bool pending_ = false;
};

}  // namespace codeline::mqtt
