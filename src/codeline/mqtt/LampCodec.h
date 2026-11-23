// Text codec for Appliance:LAMP over MQTT (docs/bindings/MQTT.md section 3,
// ON/OFF, root LAMP): /cmd and /state are both ON or (ON).
#pragma once

#include <string_view>

#include "codeline/mqtt/ApplianceCodec.h"

namespace codeline::mqtt {

inline Frame encodeOnOff(bool on) {
  Frame f;
  if (!on) f.append("(");
  f.append(kOnOffToken);
  if (!on) f.append(")");
  return f;
}

// Exactly one token; anything else is rejected.
inline bool decodeOnOff(std::string_view payload, bool& on) {
  Tokens t(payload);
  std::string_view a, extra;
  if (!t.next(a) || t.next(extra)) return false;
  return decodeAssertion(a, kOnOffToken, on);
}

}  // namespace codeline::mqtt
