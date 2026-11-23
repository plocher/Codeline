// Text codec for Appliance:CONTROL over MQTT (docs/bindings/MQTT.md sections
// 2-3 and 5): a BitVector is the complete ordered vector of its channels,
//   /state  SW1 (SW2) ... SW<N>
//   /cmd    LAMP1 (LAMP2) ... LAMP<M>
// All-or-nothing: a missing, extra, repeated, out-of-order or unknown token
// rejects the whole frame. ButtonEvent (BTN<k>) is in ApplianceCodec.h.
#pragma once

#include <string_view>

#include "codeline/appliance/IControl.h"
#include "codeline/mqtt/ApplianceCodec.h"

namespace codeline::mqtt {

// 32 tokens of "(LAMP32)" plus spaces exceed Frame.
using VectorFrame = FixedString<383>;

inline VectorFrame encodeVector(std::string_view prefix, BitVector v) {
  VectorFrame f;
  for (uint8_t k = 1; k <= v.count; ++k) {
    if (k > 1) f.append(" ");
    appendIndexed(f, prefix, k, v.get(k));
  }
  return f;
}

// An empty payload is the empty vector (count 0), which only a LEVER with no
// outputs can use.
inline bool decodeVector(std::string_view payload, std::string_view prefix, BitVector& out) {
  Tokens t(payload);
  std::string_view token;
  BitVector v;
  while (t.next(token)) {
    uint8_t channel = 0;
    bool asserted = false;
    if (v.count == BitVector::kMaxCount || !decodeIndexed(token, prefix, channel, asserted) ||
        channel != v.count + 1) {
      return false;
    }
    v.count = channel;
    v.set(channel, asserted);
  }
  out = v;
  return true;
}

inline VectorFrame encodeSwitches(BitVector v) { return encodeVector(kLeverSwitchToken, v); }
inline VectorFrame encodeLamps(BitVector v) { return encodeVector(kLeverLampToken, v); }

// A switch frame always has at least one channel (lever.inputs >= 1).
inline bool decodeSwitches(std::string_view payload, BitVector& out) {
  BitVector v;
  if (!decodeVector(payload, kLeverSwitchToken, v) || v.count == 0) return false;
  out = v;
  return true;
}
inline bool decodeLamps(std::string_view payload, BitVector& out) { return decodeVector(payload, kLeverLampToken, out); }

}  // namespace codeline::mqtt
