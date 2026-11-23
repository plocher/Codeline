// Text codec for Appliance:TURNOUT over MQTT: the NORMAL / REVERSE pair of
// docs/bindings/MQTT.md sections 2-3. The shared parts (attribute maps,
// /maint, /event) are in ApplianceCodec.h.
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/mqtt/ApplianceCodec.h"

namespace codeline::mqtt {

// ---- positive-control pair -------------------------------------------------

// Exactly "<NORMAL|(NORMAL)> <REVERSE|(REVERSE)>"; returns the index into
// `pairs` or -1 (including for the contradictory NORMAL REVERSE).
inline int decodePair(std::string_view payload, const PairBits* pairs, uint8_t count) {
  Tokens t(payload);
  std::string_view a, b, extra;
  PairBits bits{};
  if (!t.next(a) || !t.next(b) || t.next(extra)) return -1;
  if (!decodeAssertion(a, kTurnoutPairFirst, bits.first)) return -1;
  if (!decodeAssertion(b, kTurnoutPairSecond, bits.second)) return -1;
  for (uint8_t i = 0; i < count; ++i) {
    if (pairs[i].first == bits.first && pairs[i].second == bits.second) return i;
  }
  return -1;
}

inline const char* encodeCommand(TurnoutCommand c) { return kTurnoutCommandFrames[static_cast<uint8_t>(c)]; }
inline const char* encodeState(TurnoutState s) { return kTurnoutStateFrames[static_cast<uint8_t>(s)]; }

inline bool decodeCommand(std::string_view payload, TurnoutCommand& out) {
  const int i = decodePair(payload, kTurnoutCommandPairs, kTurnoutCommandCount);
  if (i < 0) return false;
  out = static_cast<TurnoutCommand>(i);
  return true;
}

inline bool decodeState(std::string_view payload, TurnoutState& out) {
  const int i = decodePair(payload, kTurnoutStatePairs, kTurnoutStateCount);
  if (i < 0) return false;
  out = static_cast<TurnoutState>(i);
  return true;
}

}  // namespace codeline::mqtt
