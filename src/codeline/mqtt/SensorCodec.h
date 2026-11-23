// Text codec for Appliance:SENSOR over MQTT (docs/bindings/MQTT.md section 3):
// /state is ACTIVE or (ACTIVE). SENSOR has no /cmd.
#pragma once

#include <string_view>

#include "codeline/mqtt/ApplianceCodec.h"

namespace codeline::mqtt {

inline Frame encodeSensorState(bool active) {
  Frame f;
  if (!active) f.append("(");
  f.append(kSensorActiveToken);
  if (!active) f.append(")");
  return f;
}

// Exactly one token; anything else is rejected.
inline bool decodeSensorState(std::string_view payload, bool& active) {
  Tokens t(payload);
  std::string_view a, extra;
  if (!t.next(a) || t.next(extra)) return false;
  return decodeAssertion(a, kSensorActiveToken, active);
}

}  // namespace codeline::mqtt
