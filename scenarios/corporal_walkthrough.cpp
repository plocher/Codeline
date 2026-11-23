// CP Corporal end to end, in process: the station built from its station file
// (build/Corporal.station, written by tools/station_file.py from the project
// file), real cores over simulated drivers behind their MQTT adapters, the
// engine on the MQTT proxies, and the code line binding, all on one
// InMemoryMockBroker with injected time. The office and the physical world
// replay scenarios/corporal_walkthrough.script; every message on the topics
// the script names, every appliance command and every indication frame is
// compared, in order, with the script's strings (the Walkthrough's, or listed
// differences); an unscripted one fails. The time locking is the station
// file's (30 s).
//
// Then every indication frame of that run is held to README.CodeLine section 3,
// "co-required levers": a lit LEFT/RIGHT (GK) on a lever is always one of a
// route's levers whose GKs are all lit (830WGK never without 828WGK), TIME
// (TEK) lights only on a route's governing lever, and never together with
// LEFT or RIGHT on the same lever.
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/station/StationHost.h"
#include "codeline/testing/WireScript.h"
#include "scenario.h"

using namespace codeline;
using namespace codeline::testing;

namespace {

struct Run {
  bool ok = false;
  station::StationSpec spec;
  std::vector<std::string> frames;  // every indication frame published, in order
};

// The Walkthrough run, once; both scenarios read it.
const Run& walkthrough() {
  static Run run;
  static bool done = false;
  if (done) return run;
  done = true;
  std::vector<WireLine> script;
  std::string error;
  const bool loaded = station::loadStationFile("build/Corporal.station", run.spec, error) &&
                      parseWireScript("scenarios/corporal_walkthrough.script", script, error);
  if (!loaded) {
    std::printf("    %s\n", error.c_str());
    return run;
  }
  mqtt::InMemoryMockBroker broker;
  station::HostConfig config;
  for (const WireLine& w : script)
    if (w.directive == "initial") config.initial.emplace_back(w.topic, w.payload);
  station::StationHost host(run.spec, config, [&broker](const std::string&) {
    return std::make_unique<mqtt::InMemoryMockBroker::Client>(broker);
  });
  if (!host.start(error)) {
    std::printf("    %s\n", error.c_str());
    return run;
  }
  mqtt::InMemoryMockBroker::Client office(broker);
  office.connect("office", nullptr);
  const WireChannels channels = {{"appliances", run.spec.appliance_channel},
                                 {"codeline", run.spec.codeline_channel},
                                 {"simulation", run.spec.simulation_channel}};
  MockWireRunner runner(broker, office, channels, [&host](uint32_t now) { host.tick(now); }, 1000,
                        run.spec.levers.front().time_locking_ms);
  run.ok = runner.run(script);
  std::printf("    %d listed differences from the Walkthrough\n", runner.listedDifferences());
  const auto& n = host.field().counts();
  std::printf("    control frames: %u applied, %u vital ignored, %u malformed, %u out of sync\n", n.applied,
              n.vital_ignored, n.malformed, n.out_of_sync);
  run.ok = run.ok && n.malformed == 0 && n.out_of_sync == 0;
  const std::string indications =
      station::stationTopic(run.spec.codeline_channel, run.spec.chart.station) + "/indications";
  for (const auto& m : broker.log())
    if (m.topic == indications) run.frames.push_back(m.payload);
  return run;
}

}  // namespace

SCENARIO("CP Corporal Walkthrough over MQTT in process: same traffic and indications, in the same order") {
  CHECK(walkthrough().ok);
}

SCENARIO("co-required levers in every frame of the run: GK on all of a route's levers together, TEK only on the governing lever, never with its GK") {
  const Run& run = walkthrough();
  CHECK(!run.frames.empty());
  using Key = std::pair<std::string, engine::Word>;  // lever, LEFT/RIGHT/TIME
  std::map<std::string, Key> token;                 // AAR token -> function
  for (const auto& f : run.spec.chart.indications)
    if (f.cls == engine::FunctionClass::Signal) token[f.aar] = Key{f.appliance, f.word};
  auto word = [](engine::Direction d) { return d == engine::Direction::Left ? engine::Word::Left : engine::Word::Right; };
  std::set<std::string> governing;
  for (const auto& r : run.spec.routes) governing.insert(r.levers.front().first);

  size_t both_gk = 0, tek = 0, bad = 0;
  std::set<std::string> tek_levers;  // after boot: the startup time locking lights every governing lever's TEK
  bool settled = false;               // a frame with no TEK lit has been seen
  for (const std::string& frame : run.frames) {
    if (frame == "UNAVAILABLE") continue;
    std::set<Key> lit;
    size_t i = 0;
    while (i < frame.size()) {
      const size_t j = frame.find(' ', i) == std::string::npos ? frame.size() : frame.find(' ', i);
      const auto t = token.find(frame.substr(i, j - i));  // a dropped "(...)" term is never found
      if (t != token.end()) lit.insert(t->second);
      i = j + 1;
    }
    std::string why;
    for (const Key& k : lit) {
      if (k.second == engine::Word::Time) {
        ++tek;
        if (settled) tek_levers.insert(k.first);
        if (!governing.count(k.first)) why += " TEK on " + k.first + ", which governs no route;";
        if (lit.count({k.first, engine::Word::Left}) || lit.count({k.first, engine::Word::Right}))
          why += " TEK with LEFT/RIGHT on " + k.first + ";";
        continue;
      }
      bool covered = false;  // some route requiring k has every one of its levers' GK lit
      for (const auto& r : run.spec.routes) {
        bool requires_k = false, all = true;
        for (const auto& l : r.levers) {
          requires_k = requires_k || Key{l.first, word(l.second)} == k;
          all = all && lit.count({l.first, word(l.second)});
        }
        covered = covered || (requires_k && all);
      }
      if (!covered) why += " " + k.first + (k.second == engine::Word::Left ? " LEFT" : " RIGHT") +
                           " lit without every other lever of a route requiring it;";
    }
    if (lit.count({"830", engine::Word::Left}) && lit.count({"828", engine::Word::Left})) ++both_gk;
    bool any_tek = false;
    for (const Key& k : lit) any_tek = any_tek || k.second == engine::Word::Time;
    settled = settled || !any_tek;
    if (!why.empty()) {
      ++bad;
      std::printf("    frame \"%s\":%s\n", frame.c_str(), why.c_str());
    }
  }
  std::printf("    %zu frames: %zu with 830WGK and 828WGK lit together, %zu TEK lamps lit; after boot TEK on:", run.frames.size(),
              both_gk, tek);
  for (const auto& l : tek_levers) std::printf(" %s", l.c_str());
  std::printf("\n");
  CHECK(bad == 0);
  CHECK(both_gk > 0);                                           // the run does clear 830WAB
  CHECK(tek_levers == std::set<std::string>{"830"});  // after boot this run times 830WAB only: never 828TEK
}

int main() { return codeline::scenario::run(); }
