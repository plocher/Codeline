// Text codec for Appliance:SIGNALMAST over MQTT (docs/bindings/MQTT.md
// sections 2-3):
//   /cmd    <INDICATION>          setIndication(indication, lit = true)
//           <INDICATION> LIT      setIndication(indication, true)
//           <INDICATION> (LIT)    setIndication(indication, false)
//           LIT | (LIT)           setLit(bool)
//   /state  <INDICATION> | <INDICATION> (LIT)
// <INDICATION> is a SignalIndication name. Decoders reject, never guess.
#pragma once

#include <string_view>

#include "codeline/appliance/ISignalMast.h"
#include "codeline/mqtt/ApplianceCodec.h"

namespace codeline::mqtt {

// The proxy writes the lit = true form as the bare indication, as the
// Walkthrough does ("APPROACH").
inline Frame encodeMastCommand(SignalIndication indication, bool lit) {
  Frame f;
  f.append(nameOf(indication));
  if (!lit) f.append(" (").append(kMastLitToken).append(")");
  return f;
}

inline Frame encodeLitCommand(bool lit) {
  Frame f;
  if (!lit) f.append("(");
  f.append(kMastLitToken);
  if (!lit) f.append(")");
  return f;
}

inline Frame encodeMastState(MastState s) { return encodeMastCommand(s.indication, s.lit); }

struct MastCommand {
  bool has_indication = false;  // false: setLit() alone
  SignalIndication indication = SignalIndication::Stop;
  bool lit = true;
};

// Splits "<a> [<b>]" into one or two tokens; false for none or more than two.
inline bool splitOneOrTwo(std::string_view payload, std::string_view& first, std::string_view& second) {
  Tokens t(payload);
  std::string_view extra;
  second = std::string_view();
  if (!t.next(first)) return false;
  t.next(second);
  return !t.next(extra);
}

inline bool decodeMastCommand(std::string_view payload, MastCommand& out) {
  std::string_view first, second;
  if (!splitOneOrTwo(payload, first, second)) return false;
  MastCommand c;
  if (parseName(first, c.indication)) {
    c.has_indication = true;
    if (!second.empty() && !decodeAssertion(second, kMastLitToken, c.lit)) return false;
  } else if (second.empty() && decodeAssertion(first, kMastLitToken, c.lit)) {
    c.has_indication = false;
  } else {
    return false;
  }
  out = c;
  return true;
}

inline bool decodeMastState(std::string_view payload, MastState& out) {
  std::string_view first, second;
  if (!splitOneOrTwo(payload, first, second)) return false;
  MastState s;
  if (!parseName(first, s.indication)) return false;
  if (!second.empty()) {
    bool lit = true;
    if (!decodeAssertion(second, kMastLitToken, lit) || lit) return false;  // only "(LIT)" may follow
    s.lit = false;
  }
  out = s;
  return true;
}

}  // namespace codeline::mqtt
