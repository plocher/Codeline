// Text codec shared by every appliance over MQTT: the TOKEN / (TOKEN)
// assertion, attribute values and maps, /maint frames and /event frames of
// docs/bindings/MQTT.md sections 2-3. Every token comes from the generated
// tables in codeline/generated/MqttTables.h. Decoders reject, never guess.
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/appliance/IControl.h"
#include "codeline/appliance/IMaintenance.h"
#include "codeline/core/Attributes.h"
#include "codeline/core/Text.h"
#include "codeline/generated/MqttTables.h"

namespace codeline::mqtt {

using Frame = FixedString<255>;

// ---- positive-control token ------------------------------------------------

// One token of a pair: "NORMAL" asserts, "(NORMAL)" negates.
inline bool decodeAssertion(std::string_view token, std::string_view name, bool& asserted) {
  if (token == name) {
    asserted = true;
    return true;
  }
  if (token.size() == name.size() + 2 && token.front() == '(' && token.back() == ')' &&
      token.substr(1, name.size()) == name) {
    asserted = false;
    return true;
  }
  return false;
}

// One token of an indexed vector or event: "SW3" asserts channel 3, "(SW3)"
// negates it. Channels are 1..BitVector::kMaxCount, written without leading zeros.
inline bool decodeIndexed(std::string_view token, std::string_view prefix, uint8_t& channel, bool& asserted) {
  asserted = !(token.size() >= 2 && token.front() == '(' && token.back() == ')');
  if (!asserted) token = token.substr(1, token.size() - 2);
  if (token.size() <= prefix.size() || token.substr(0, prefix.size()) != prefix) return false;
  const std::string_view digits = token.substr(prefix.size());
  uint32_t n = 0;
  if (digits.front() == '0' || !parseUint(digits, BitVector::kMaxCount, n) || n == 0) return false;
  channel = static_cast<uint8_t>(n);
  return true;
}

template <size_t N>
void appendIndexed(FixedString<N>& out, std::string_view prefix, uint8_t channel, bool asserted) {
  if (!asserted) out.append("(");
  out.append(prefix).append(uint32_t{channel});
  if (!asserted) out.append(")");
}

// ---- attribute values and maps ("key=value key=value") ---------------------

template <size_t N>
void appendValue(FixedString<N>& out, const AttributeInfo& a, AttrValue v) {
  switch (a.type) {
    case AttrType::Bool: out.append(v.raw ? kBoolTrue : kBoolFalse); break;
    case AttrType::Enum: out.append(a.enum_names[v.raw]); break;
    case AttrType::Uint: out.append(v.raw); break;
    case AttrType::Hundredths: {
      const uint32_t frac = v.raw % 100;
      out.append(v.raw / 100).append(".").append(frac < 10 ? "0" : "").append(frac);
      break;
    }
  }
}

inline bool parseValue(const AttributeInfo& a, std::string_view s, AttrValue& out) {
  switch (a.type) {
    case AttrType::Bool:
      if (s == kBoolTrue) out = AttrValue::boolean(true);
      else if (s == kBoolFalse) out = AttrValue::boolean(false);
      else return false;
      return true;
    case AttrType::Enum: {
      const int i = lookupToken(a.enum_names, static_cast<uint8_t>(a.max_value + 1), s);
      if (i < 0) return false;
      out = AttrValue{static_cast<uint32_t>(i)};
      return true;
    }
    case AttrType::Uint: return parseUint(s, a.max_value, out.raw) && isValid(a, out);
    case AttrType::Hundredths: {
      // "1", "1.0" or "1.00": at most two fractional digits.
      const size_t dot = s.find('.');
      const std::string_view whole = s.substr(0, dot);
      const std::string_view frac = dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);
      if (dot != std::string_view::npos && (frac.empty() || frac.size() > 2)) return false;
      uint32_t w = 0, f = 0;
      if (!parseUint(whole, a.max_value / 100, w)) return false;
      if (!frac.empty() && !parseUint(frac, 99, f)) return false;
      if (frac.size() == 1) f *= 10;
      out.raw = w * 100 + f;
      return isValid(a, out);
    }
  }
  return false;
}

template <size_t N>
void appendMap(FixedString<N>& out, const AttrMap& map) {
  for (uint8_t i = 0; i < map.count; ++i) {
    if (i) out.append(" ");
    out.append(map.entries[i].info->path).append("=");
    appendValue(out, *map.entries[i].info, map.entries[i].value);
  }
}

// Any unknown key or bad value rejects the whole map.
inline bool parseMap(const AttributeTable& table, std::string_view s, AttrMap& out) {
  out.count = 0;
  Tokens t(s);
  std::string_view item;
  while (t.next(item)) {
    const size_t eq = item.find('=');
    if (eq == std::string_view::npos) return false;
    const AttributeInfo* a = table.find(item.substr(0, eq));
    AttrValue v;
    if (!a || !parseValue(*a, item.substr(eq + 1), v) || !out.add(a, v)) return false;
  }
  return true;
}

// ---- /maint and {channel}/all ----------------------------------------------

struct MaintFrame {
  MaintenanceCommand command = MaintenanceCommand::Identify;
  IdentifyMode mode = IdentifyMode::Off;  // Identify
  uint32_t duration_ms = 0;               // Identify PULSE; Cycle dwell
  uint16_t count = 0;                     // Cycle
  const AttributeInfo* attribute = nullptr;  // SetAttribute, ClearAttribute
  SetOp op = SetOp::Assign;
  AttrValue value;
  AttrMap config;  // ImportConfig
};

// Longest verb that is the whole payload or is followed by a space.
inline int matchVerb(std::string_view payload, std::string_view& args) {
  int best = -1;
  size_t best_len = 0;
  for (uint8_t i = 0; i < kMaintenanceCommandCount; ++i) {
    const std::string_view verb = kMaintenanceVerbs[i];
    const bool whole = payload == verb;
    const bool prefix = payload.size() > verb.size() && payload.substr(0, verb.size()) == verb &&
                        payload[verb.size()] == ' ';
    if ((whole || prefix) && verb.size() > best_len) {
      best = i;
      best_len = verb.size();
    }
  }
  if (best >= 0) args = trim(payload.substr(best_len));
  return best;
}

inline bool decodeMaint(std::string_view payload, const AttributeTable& table, MaintFrame& f) {
  std::string_view args;
  const int verb = matchVerb(trim(payload), args);
  if (verb < 0) return false;
  f.command = static_cast<MaintenanceCommand>(verb);
  switch (f.command) {
    case MaintenanceCommand::Identify:
      if (parseName(args, f.mode) && f.mode != IdentifyMode::Pulse) return true;
      f.mode = IdentifyMode::Pulse;
      return parseUint(args, UINT32_MAX, f.duration_ms);
    case MaintenanceCommand::ExportAttributes:
    case MaintenanceCommand::ExportConfig:
    case MaintenanceCommand::Offline:
    case MaintenanceCommand::Online:
    case MaintenanceCommand::ClearFault:
      return args.empty();
    case MaintenanceCommand::SetAttribute: {
      // "path = v", "path += v", "path -= v"; spaces around the operator optional.
      size_t at = args.find("+=");
      if (at == std::string_view::npos) at = args.find("-=");
      if (at == std::string_view::npos) at = args.find('=');
      if (at == std::string_view::npos) return false;
      const size_t op_len = args[at] == '=' ? 1 : 2;
      const int op = lookupToken(kSetOpTokens, kSetOpCount, args.substr(at, op_len));
      f.attribute = table.find(trim(args.substr(0, at)));
      if (op < 0 || !f.attribute) return false;
      f.op = static_cast<SetOp>(op);
      return parseValue(*f.attribute, trim(args.substr(at + op_len)), f.value);
    }
    case MaintenanceCommand::ClearAttribute:
      f.attribute = table.find(args);
      return f.attribute != nullptr;
    case MaintenanceCommand::ImportConfig:
      // An IMPORT whose map does not decode is rejected whole, as a core
      // rejects a map with one bad entry: nothing is written, no CONFIG answers.
      return parseMap(table, args, f.config) && f.config.count > 0;
    case MaintenanceCommand::Cycle: {
      Tokens t(args);
      std::string_view count, dwell, extra;
      uint32_t n = 0;
      if (!t.next(count) || !t.next(dwell) || t.next(extra)) return false;
      if (!parseUint(count, UINT16_MAX, n) || !parseUint(dwell, UINT32_MAX, f.duration_ms)) return false;
      f.count = static_cast<uint16_t>(n);
      return true;
    }
  }
  return false;
}

inline void applyMaint(const MaintFrame& f, IMaintenance& target) {
  switch (f.command) {
    case MaintenanceCommand::Identify: target.identify(f.mode, f.duration_ms); break;
    case MaintenanceCommand::ExportAttributes: target.exportAttributes(); break;
    case MaintenanceCommand::ExportConfig: target.exportConfig(); break;
    case MaintenanceCommand::Offline: target.offline(); break;
    case MaintenanceCommand::Online: target.online(); break;
    case MaintenanceCommand::SetAttribute: target.setAttribute(f.attribute->path, f.op, f.value); break;
    case MaintenanceCommand::ClearAttribute: target.clearAttribute(f.attribute->path); break;
    case MaintenanceCommand::ClearFault: target.clearFault(); break;
    case MaintenanceCommand::ImportConfig: target.importConfig(f.config); break;
    case MaintenanceCommand::Cycle: target.cycle(f.count, f.duration_ms); break;
  }
}

inline const char* verb(MaintenanceCommand c) { return kMaintenanceVerbs[static_cast<uint8_t>(c)]; }

// ---- /event ------------------------------------------------------------------

// False for events that have no wire form (UnavailableEvent). A ButtonEvent
// is BTN<k> when pressed and (BTN<k>) when released.
inline bool encodeEvent(const ApplianceEvent& ev, Frame& out) {
  const char* noun = kEventNouns[static_cast<uint8_t>(ev.kind)];
  if (!noun) return false;
  out.clear();
  if (ev.kind == EventKind::Button) {
    if (ev.channel < 1 || ev.channel > BitVector::kMaxCount) return false;
    appendIndexed(out, kButtonToken, ev.channel, ev.pressed);
    return out.ok();
  }
  out.append(noun);
  if (ev.kind == EventKind::Fault) {
    out.append(" ").append(nameOf(ev.fault));
    if (!ev.detail.empty()) out.append(" (").append(ev.detail).append(")");
  } else if (ev.data && ev.data->count > 0) {
    out.append(" ");
    appendMap(out, *ev.data);
  }
  return out.ok();
}

// `map` receives ATTRIBUTES / CONFIG data; ev.detail views into `payload`.
// `events` (the generated k<Name>Events, indexed by EventKind) says which
// events the appliance emits; any other decodes as false.
inline bool decodeEvent(std::string_view payload, const AttributeTable& table, const bool* events, ApplianceEvent& ev,
                        AttrMap& map) {
  Tokens t(payload);
  std::string_view noun;
  if (!t.next(noun)) return false;
  uint8_t channel = 0;
  bool pressed = false;
  if (decodeIndexed(noun, kButtonToken, channel, pressed)) {
    if (!t.rest().empty() || !events[static_cast<uint8_t>(EventKind::Button)]) return false;
    ev = ApplianceEvent::of(EventKind::Button);
    ev.channel = channel;
    ev.pressed = pressed;
    return true;
  }
  const int kind = lookupToken(kEventNouns, kEventKindCount, noun);
  if (kind < 0 || !events[kind]) return false;
  ev = ApplianceEvent::of(static_cast<EventKind>(kind));
  std::string_view rest = t.rest();
  switch (ev.kind) {
    case EventKind::Fault: {
      Tokens ft(rest);
      std::string_view type;
      if (!ft.next(type) || !parseName(type, ev.fault)) return false;
      rest = ft.rest();
      if (rest.empty()) return true;
      if (rest.size() < 2 || rest.front() != '(' || rest.back() != ')') return false;
      ev.detail = rest.substr(1, rest.size() - 2);
      return true;
    }
    case EventKind::Attributes:
    case EventKind::Config:
      ev.data = &map;
      return parseMap(table, rest, map);
    case EventKind::Button:
      return false;  // only the indexed BTN<k> form above
    default:
      return rest.empty();
  }
}

}  // namespace codeline::mqtt
