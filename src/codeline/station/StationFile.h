// Reads a station file written by tools/station_file.py: one station's chart
// (from the one chart deriver, tools/codeline_chart.py) and its plant, as
// flat records of space-separated fields. The grammar is in the docstring of
// tools/station_file.py. Nothing here derives anything: the chart's order,
// its identity and its positive-control groups are taken as written.
//
// Errors name the line. A runtime refuses a station file it cannot read
// whole; it never guesses a missing field.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codeline/core/Text.h"
#include "codeline/engine/Office.h"
#include "codeline/engine/Types.h"
#include "codeline/generated/Enums.h"

namespace codeline::station {

// ---- the chart ---------------------------------------------------------------

struct ChartFunction {
  engine::FunctionClass cls;
  std::string appliance;
  engine::Word word;
  std::string aar;  // its AAR token
  int group;        // positive-control group within the station; 0: none
};

struct Chart {
  std::string station;
  std::string id;  // chart identity, 8 lowercase hex digits
  std::vector<ChartFunction> controls;
  std::vector<ChartFunction> indications;
};

// ---- the plant -----------------------------------------------------------------

struct NvSetting {
  std::string path;
  std::string value;  // a number, or an enum name of the attribute
};

struct CircuitSpec {
  std::string name, role, id;
};

struct SwitchSpec {
  std::string name;
  bool power = false;  // the engine commands it; otherwise it only reads it
  std::string role, id;
  std::string os;  // empty: none
  uint32_t travel_timeout_ms = 0;
};

struct LockSpec {
  std::string name, role, id;
  std::vector<std::string> switches;
  std::vector<NvSetting> nv;
};

struct LeverSpec {
  std::string name;
  uint32_t time_locking_ms = 0;
};

struct MastSpec {
  std::string name, role, id;
  std::vector<NvSetting> nv;
};

struct AlignSpec {
  std::string sw;
  engine::Position pos;
  std::string releasing;  // empty: none
};

struct RouteSpec {
  std::string name, mast;
  std::vector<SignalIndication> presents, caps;
  std::vector<std::pair<std::string, engine::Direction>> levers;  // the first governs
  std::vector<AlignSpec> align;
  std::vector<std::string> clears;
  std::string approach;  // empty: none
};

struct StationSpec {
  Chart chart;
  std::string source;
  std::string appliance_channel, codeline_channel, simulation_channel;
  std::vector<CircuitSpec> circuits;
  std::vector<SwitchSpec> switches;
  std::vector<LockSpec> locks;
  std::vector<LeverSpec> levers;
  std::vector<MastSpec> masts;
  std::vector<std::string> calls;
  std::vector<CircuitSpec> contacts;        // a contact shown to the office as a lamp
  std::vector<std::string> unbound_lamps;   // chart lamps with no source yet: dark
  std::vector<RouteSpec> routes;
};

// ---- vocabulary --------------------------------------------------------------------

inline bool parseClass(std::string_view s, engine::FunctionClass& out) {
  using C = engine::FunctionClass;
  static constexpr std::pair<const char*, C> kNames[] = {
      {"switch", C::Switch}, {"lock", C::Lock}, {"signal", C::Signal},
      {"maintainer_call", C::MaintainerCall}, {"track_circuit", C::TrackCircuit}, {"lamp", C::Lamp}};
  for (const auto& n : kNames)
    if (s == n.first) return out = n.second, true;
  return false;
}

inline bool parseWord(std::string_view s, engine::Word& out) {
  using W = engine::Word;
  static constexpr std::pair<const char*, W> kNames[] = {
      {"NORMAL", W::Normal},     {"REVERSE", W::Reverse},   {"LEFT", W::Left},       {"RIGHT", W::Right},
      {"STOP", W::Stop},         {"TIME", W::Time},         {"RELEASE", W::Release}, {"RELEASED", W::Released},
      {"REQUEST", W::Request},   {"CALL", W::Call},         {"OCCUPIED", W::Occupied}, {"LIT", W::Lit}};
  for (const auto& n : kNames)
    if (s == n.first) return out = n.second, true;
  return false;
}

// ---- parsing ---------------------------------------------------------------------

namespace detail {

inline std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    const bool brk = sep == ' ' ? (c == ' ' || c == '\t' || c == '\r') : c == sep;
    if (brk) {
      if (sep != ' ' || !cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (sep != ' ' || !cur.empty()) out.push_back(cur);
  return out;
}

class Line {
public:
  Line(int number, std::vector<std::string> fields) : number_(number), fields_(std::move(fields)) {}

  // Positional field i (0 is the keyword).
  bool at(size_t i, std::string& out) const {
    if (i >= positional()) return false;
    out = fields_[i];
    return true;
  }
  size_t positional() const {
    size_t n = 0;
    while (n < fields_.size() && fields_[n].find('=') == std::string::npos) ++n;
    return n;
  }
  // key=value field; "-" is returned as empty.
  bool get(std::string_view key, std::string& out) const {
    for (const std::string& f : fields_) {
      const size_t eq = f.find('=');
      if (eq != std::string::npos && std::string_view(f).substr(0, eq) == key) {
        out = f.substr(eq + 1);
        if (out == "-") out.clear();
        return true;
      }
    }
    return false;
  }
  int number() const { return number_; }

private:
  int number_;
  std::vector<std::string> fields_;
};

inline bool fail(std::string& error, int line, const std::string& what) {
  error = "station file line " + std::to_string(line) + ": " + what;
  return false;
}

inline bool parseUintField(const std::string& s, uint32_t& out) { return parseUint(s, 0xFFFFFFFFu, out); }

inline std::vector<std::string> list(const std::string& s) {
  return s.empty() ? std::vector<std::string>{} : split(s, ',');
}

inline bool parseNv(const std::string& s, std::vector<NvSetting>& out) {
  for (const std::string& item : list(s)) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == item.size()) return false;
    out.push_back(NvSetting{item.substr(0, colon), item.substr(colon + 1)});
  }
  return true;
}

inline bool parseIndications(const std::string& s, std::vector<SignalIndication>& out) {
  for (const std::string& item : list(s)) {
    SignalIndication i;
    if (!parseName(item, i)) return false;
    out.push_back(i);
  }
  return true;
}

inline bool parseFunction(const Line& l, ChartFunction& f) {
  std::string cls, appliance, word, aar, group;
  uint32_t g = 0;
  if (l.positional() != 6 || !l.at(1, cls) || !l.at(2, appliance) || !l.at(3, word) || !l.at(4, aar) ||
      !l.at(5, group) || !parseUintField(group, g))
    return false;
  f.appliance = appliance;
  f.aar = aar;
  f.group = static_cast<int>(g);
  return parseClass(cls, f.cls) && parseWord(word, f.word);
}

inline bool parseRoute(const Line& l, RouteSpec& r) {
  std::string presents, caps, levers, align, clears;
  if (l.positional() != 2 || !l.at(1, r.name) || !l.get("mast", r.mast) || !l.get("presents", presents) ||
      !l.get("caps", caps) || !l.get("levers", levers) || !l.get("align", align) || !l.get("clears", clears) ||
      !l.get("approach", r.approach))
    return false;
  if (!parseIndications(presents, r.presents) || !parseIndications(caps, r.caps)) return false;
  for (const std::string& item : list(levers)) {
    const std::vector<std::string> p = split(item, ':');
    if (p.size() != 2 || (p[1] != "LEFT" && p[1] != "RIGHT")) return false;
    r.levers.emplace_back(p[0], p[1] == "LEFT" ? engine::Direction::Left : engine::Direction::Right);
  }
  for (const std::string& item : list(align)) {
    const std::vector<std::string> p = split(item, ':');
    if (p.size() != 3 || (p[1] != "NORMAL" && p[1] != "REVERSE")) return false;
    r.align.push_back(AlignSpec{p[0], p[1] == "NORMAL" ? engine::Position::Normal : engine::Position::Reverse,
                                p[2] == "-" ? std::string() : p[2]});
  }
  r.clears = list(clears);
  return !r.levers.empty() && !r.clears.empty();
}

}  // namespace detail

// Parses a whole station file. Every record must parse; unknown keywords are errors.
inline bool parseStation(std::string_view text, StationSpec& out, std::string& error) {
  using detail::fail;
  StationSpec s;
  int number = 0;
  for (const std::string& raw : detail::split(text, '\n')) {
    ++number;
    const std::vector<std::string> fields = detail::split(raw, ' ');
    if (fields.empty() || fields[0][0] == '#') continue;
    const detail::Line l(number, fields);
    const std::string& kw = fields[0];
    std::string a, b, c, d, v;
    if (kw == "station") {
      if (l.positional() != 2 || !l.at(1, s.chart.station)) return fail(error, number, "station <name>");
    } else if (kw == "source") {
      if (!l.at(1, s.source)) return fail(error, number, "source <path> <digest>");
    } else if (kw == "chart") {
      if (l.positional() != 2 || !l.at(1, s.chart.id)) return fail(error, number, "chart <id>");
    } else if (kw == "control" || kw == "indication") {
      ChartFunction f;
      if (!detail::parseFunction(l, f)) return fail(error, number, "bad chart function");
      (kw == "control" ? s.chart.controls : s.chart.indications).push_back(f);
    } else if (kw == "channel") {
      if (l.positional() != 3 || !l.at(1, a) || !l.at(2, b)) return fail(error, number, "channel <which> <topic>");
      if (a == "appliances") s.appliance_channel = b;
      else if (a == "codeline") s.codeline_channel = b;
      else if (a == "simulation") s.simulation_channel = b;
      else return fail(error, number, "unknown channel " + a);
    } else if (kw == "circuit") {
      CircuitSpec cs;
      if (l.positional() != 4 || !l.at(1, cs.name) || !l.at(2, cs.role) || !l.at(3, cs.id))
        return fail(error, number, "circuit <name> <role> <id>");
      s.circuits.push_back(cs);
    } else if (kw == "switch") {
      SwitchSpec sw;
      if (l.positional() != 5 || !l.at(1, sw.name) || !l.at(2, a) || !l.at(3, sw.role) || !l.at(4, sw.id) ||
          (a != "power" && a != "hand") || !l.get("os", sw.os) || !l.get("travel_timeout_ms", v) ||
          !detail::parseUintField(v, sw.travel_timeout_ms))
        return fail(error, number, "switch <name> <power|hand> <role> <id> os= travel_timeout_ms=");
      sw.power = a == "power";
      s.switches.push_back(sw);
    } else if (kw == "lock") {
      LockSpec lk;
      if (l.positional() != 4 || !l.at(1, lk.name) || !l.at(2, lk.role) || !l.at(3, lk.id) ||
          !l.get("switches", a) || !l.get("nv", b) || !detail::parseNv(b, lk.nv))
        return fail(error, number, "lock <name> <role> <id> switches= nv=");
      lk.switches = detail::list(a);
      if (lk.switches.empty()) return fail(error, number, "a lock locks no switch");
      s.locks.push_back(lk);
    } else if (kw == "lever") {
      LeverSpec lv;
      if (l.positional() != 2 || !l.at(1, lv.name) || !l.get("time_locking_ms", v) ||
          !detail::parseUintField(v, lv.time_locking_ms))
        return fail(error, number, "lever <name> time_locking_ms=");
      s.levers.push_back(lv);
    } else if (kw == "mast") {
      MastSpec m;
      if (l.positional() != 4 || !l.at(1, m.name) || !l.at(2, m.role) || !l.at(3, m.id) || !l.get("nv", a) ||
          !detail::parseNv(a, m.nv))
        return fail(error, number, "mast <name> <role> <id> nv=");
      s.masts.push_back(m);
    } else if (kw == "contact") {
      CircuitSpec cs;
      if (l.positional() != 4 || !l.at(1, cs.name) || !l.at(2, cs.role) || !l.at(3, cs.id))
        return fail(error, number, "contact <name> <role> <id>");
      s.contacts.push_back(cs);
    } else if (kw == "lamp") {
      if (l.positional() != 2 || !l.at(1, a)) return fail(error, number, "lamp <name>");
      s.unbound_lamps.push_back(a);
    } else if (kw == "call") {
      if (l.positional() != 2 || !l.at(1, a)) return fail(error, number, "call <name>");
      s.calls.push_back(a);
    } else if (kw == "route") {
      RouteSpec r;
      if (!detail::parseRoute(l, r)) return fail(error, number, "bad route record");
      if (!l.get("next", v) || !v.empty())
        return fail(error, number, "route next= must be '-': this host has no source for a next-signal fact");
      s.routes.push_back(r);
    } else {
      return fail(error, number, "unknown record " + kw);
    }
  }
  if (s.chart.station.empty() || s.chart.id.empty() || s.chart.controls.empty() || s.chart.indications.empty())
    return fail(error, number, "no station, chart, controls or indications");
  if (s.appliance_channel.empty() || s.codeline_channel.empty() || s.simulation_channel.empty())
    return fail(error, number, "channel appliances, codeline and simulation are all required");
  out = std::move(s);
  return true;
}

inline bool loadStationFile(const char* path, StationSpec& out, std::string& error) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    error = std::string("cannot open station file ") + path;
    return false;
  }
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
  std::fclose(f);
  return parseStation(text, out, error);
}

}  // namespace codeline::station
