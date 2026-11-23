// The plant derived from the project file (build/Corporal.station, written by
// tools/station_file.py) against the plant written down by hand from the same
// project file and docs/Walkthrough.md (src/codeline/testing/CorporalPlant.h),
// fact by fact, as the engine holds them: circuits; switches, their kind, OS
// circuit and lock; the lock and what it covers; levers; masts; the
// maintainer call; and every route's mast, ceiling, levers (in order),
// switches with position and releasing circuit, circuits to clear (in order,
// the first is the entrance) and approach circuit. Route names differ by
// design (the derived name spells the path), so routes are compared by their
// facts. Any fact on one side only fails the scenario, unless it is listed in
// kExplained below with its reason.
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/station/StationHost.h"
#include "codeline/testing/CorporalPlant.h"
#include "scenario.h"

using namespace codeline;

namespace {

// Differences the derivation is known to have, each with its reason. Empty:
// none today. (The DR term, nextSignal, is not compared: Route has no read
// for it. The reference wires Christopher's and 832EA's signals as constant
// STOP facts; the station file has none, STOPGAP `next`. Both cap the
// ceiling CLEAR at APPROACH while those facts are STOP.)
const std::vector<std::pair<std::string, std::string>> kExplained = {};

const char* dirName(engine::Direction d) {
  return d == engine::Direction::Left ? "LEFT" : d == engine::Direction::Right ? "RIGHT" : "NONE";
}
const char* posName(engine::Position p) {
  return p == engine::Position::Normal ? "NORMAL" : p == engine::Position::Reverse ? "REVERSE" : "?";
}
std::string nameOrDash(const engine::TrackCircuit* tc) { return tc ? tc->name() : "-"; }

struct Names {
  std::vector<std::string> circuits, switches, locks, levers, masts, calls, routes;
};

std::set<std::string> facts(engine::Plant& p, const Names& n) {
  std::set<std::string> out;
  for (const auto& c : n.circuits) out.insert("circuit " + c + (p.findTrackCircuit(c) ? "" : " (missing)"));
  for (const auto& s : n.switches) {
    engine::Switch* sw = p.findSwitch(s);
    if (sw == nullptr) {
      out.insert("switch " + s + " (missing)");
      continue;
    }
    out.insert("switch " + s + (sw->kind() == engine::Switch::Kind::Power ? " power" : " hand") +
               " os=" + nameOrDash(sw->osCircuit()) + " lock=" + (sw->electricLock() ? sw->electricLock()->name() : "-"));
  }
  for (const auto& l : n.locks) {
    engine::ElectricLock* el = p.findLock(l);
    std::string f = "lock " + l + " covers";
    if (el == nullptr) f += " (missing)";
    else {
      std::vector<std::string> sw;
      for (const engine::Switch* s : el->switches()) sw.push_back(s->name());
      std::sort(sw.begin(), sw.end());
      for (const auto& s : sw) f += " " + s;
    }
    out.insert(f);
  }
  for (const auto& l : n.levers) out.insert("lever " + l + (p.findLever(l) ? "" : " (missing)"));
  for (const auto& m : n.masts) out.insert("mast " + m + (p.findMast(m) ? "" : " (missing)"));
  for (const auto& c : n.calls) {
    bool v = false;
    out.insert("call " + c + (p.indications().get(engine::FunctionClass::MaintainerCall, c, engine::Word::Call, v)
                                  ? "" : " (missing)"));
  }
  for (const auto& rn : n.routes) {
    const engine::Route* r = p.findRoute(rn);
    if (r == nullptr) {
      out.insert("route " + rn + " (missing)");
      continue;
    }
    std::string f = "route mast=" + (r->mast() ? r->mast()->name() : std::string("-")) +
                    " ceiling=" + nameOf(r->ceiling()) + " levers=";
    for (const auto& l : r->levers()) f += l.lever->name() + ":" + dirName(l.dir) + " ";
    std::vector<std::string> sw;
    for (const auto& s : r->switches()) sw.push_back(s.sw->name() + ":" + posName(s.pos) + ":" + nameOrDash(s.releasing));
    std::sort(sw.begin(), sw.end());
    f += "align=";
    for (const auto& s : sw) f += s + " ";
    f += "clears=";
    for (const engine::TrackCircuit* tc : r->blocks()) f += tc->name() + " ";
    f += "approach=" + nameOrDash(r->approachCircuit());
    out.insert(f);
  }
  return out;
}

}  // namespace

SCENARIO("CP Corporal derived from the project file is the hand-written reference plant, fact by fact") {
  station::StationSpec spec;
  std::string error;
  const bool loaded = station::loadStationFile("build/Corporal.station", spec, error);
  if (!loaded) std::printf("    %s\n", error.c_str());
  CHECK(loaded);
  if (!loaded) return;
  mqtt::InMemoryMockBroker broker;
  station::StationHost host(spec, station::HostConfig{}, [&broker](const std::string&) {
    return std::make_unique<mqtt::InMemoryMockBroker::Client>(broker);
  });
  const bool started = host.start(error);
  if (!started) std::printf("    %s\n", error.c_str());
  CHECK(started);
  if (!started) return;

  Names derived;
  for (const auto& c : spec.circuits) derived.circuits.push_back(c.name);
  for (const auto& s : spec.switches) derived.switches.push_back(s.name);
  for (const auto& l : spec.locks) derived.locks.push_back(l.name);
  for (const auto& l : spec.levers) derived.levers.push_back(l.name);
  for (const auto& m : spec.masts) derived.masts.push_back(m.name);
  for (const auto& c : spec.calls) derived.calls.push_back(c);
  for (const auto& r : spec.routes) derived.routes.push_back(r.name);

  testing::CorporalPlant ref;  // its members are the reference's names
  Names reference;
  for (const engine::TrackCircuit* tc : {&ref.tc829T1, &ref.tc827T1, &ref.tc832T}) reference.circuits.push_back(tc->name());
  for (const engine::Switch* sw : {&ref.sw829, &ref.sw827, &ref.sw827D}) reference.switches.push_back(sw->name());
  reference.locks.push_back(ref.el827.name());
  for (const engine::SignalLever* l : {&ref.sig828, &ref.sig830}) reference.levers.push_back(l->name());
  for (const engine::SignalMast* m : {&ref.mast830WAB, &ref.mast830EAB, &ref.mast828WA, &ref.mast828EA})
    reference.masts.push_back(m->name());
  reference.calls = {"1MC"};                    // CorporalPlant() adds it
  reference.routes = {"830W", "830E", "828E"};  // CorporalPlant() adds them

  const std::set<std::string> d = facts(host.plant(), derived), r = facts(ref.cp, reference);
  int unexplained = 0;
  auto report = [&](const std::set<std::string>& a, const std::set<std::string>& b, const char* side) {
    for (const auto& f : a) {
      if (b.count(f)) continue;
      const auto e = std::find_if(kExplained.begin(), kExplained.end(), [&f](const auto& x) { return x.first == f; });
      if (e != kExplained.end()) {
        std::printf("    explained, %s only: %s (%s)\n", side, f.c_str(), e->second.c_str());
      } else {
        std::printf("    %s only: %s\n", side, f.c_str());
        ++unexplained;
      }
    }
  };
  report(d, r, "derived");
  report(r, d, "reference");
  std::printf("    %zu facts compared\n", d.size());
  CHECK(unexplained == 0);
  CHECK(d.size() == r.size());
}

int main() { return codeline::scenario::run(); }
