// The maintainer call, end to end (docs/codeline/README.CodeLine.md sections 3
// and 5.3; docs/appliances/README.Appliances.md, Appliance:LAMP).
//
// First on the engine alone: a plant whose call is bound to a real LampCore
// over a simulated lamp, beside a signal the frame's vital part can be refused
// on. Then CP Corporal built from its station file (build/Corporal.station:
// `call 1MC lamp Corporal:1MC`), every appliance behind its MQTT adapter on one
// InMemoryMockBroker: the office's control frame, the lamp's own topics, the
// indication frame, the simulation channel and the debug snapshot.
//
//   control 1MCS  ->  lamp/Corporal:1MC/cmd ON  ->  lamp/Corporal:1MC/state ON  ->  indication 1MCK
//
// 1MCK is the lamp's own report, never the stored control: a lamp that cannot
// be read shows (1MCK). The call is the one non-vital control: it is applied
// even when the vital part of its frame is refused.
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "codeline/appliance/LampCore.h"
#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/sim/SimulatedLampDriver.h"
#include "codeline/station/StationFile.h"
#include "codeline/station/StationHost.h"
#include "codeline/testing/EngineRig.h"
#include "scenario.h"

using namespace codeline;
using namespace codeline::testing;

namespace {

// ---- the engine alone ---------------------------------------------------------

// One power switch with its OS, one signal, and a maintainer call on a real lamp core.
struct CallRig {
  StandInSensor s1T, s1M;
  StandInTurnout t1;
  StandInMast m2R;
  SimulatedLampDriver bulb;
  InMemoryNvStore nv;
  LampCore lamp{bulb, nv};
  engine::ConstantFact next{true};

  engine::Plant cp{"CP_CALL"};
  engine::TrackCircuit& os = cp.addTrackCircuit("1T", s1T);
  engine::TrackCircuit& main = cp.addTrackCircuit("1M", s1M);
  engine::Switch& sw = cp.addPowerSwitch("1", t1, &os);
  engine::SignalLever& sig2 = cp.addSignalLever("2");
  engine::SignalMast& mast2 = cp.addMast("2R", m2R);
  uint32_t t = 1000;

  CallRig() {
    cp.addRoute("MAIN")
        .shows(mast2, SignalIndication::Clear)
        .governedBy(sig2, Direction::Right)
        .aligns(sw, Position::Normal, &os)
        .clears({&os, &main})
        .nextSignal(next);
    cp.addMaintainerCall("1MC", lamp);
    sig2.setTimeLockingMs(1000);
    tick(t);
    run(2500);  // everything admitted; the startup time locking expires
  }
  void tick(uint32_t now) {
    s1T.tick(now);
    s1M.tick(now);
    t1.tick(now);
    m2R.tick(now);
    lamp.tick(now);
    cp.tick(now);
  }
  void run(uint32_t ms) {
    for (const uint32_t end = t + ms; t < end;) tick(t += 100);
  }
  ControlResult code(bool call, Word sw1 = kNone, Word sig = kNone) {
    engine::ControlTransaction c = cp.newControls();
    if (sw1 != kNone) c.assertFunction(FunctionClass::Switch, "1", sw1);
    if (sig != kNone) c.assertFunction(FunctionClass::Signal, "2", sig);
    if (call) c.assertFunction(FunctionClass::MaintainerCall, "1MC", Word::Call);
    return cp.apply(c);
  }
  bool indicated() const { return cp.indications().is(FunctionClass::MaintainerCall, "1MC", Word::Call); }
};

// ---- CP Corporal over MQTT ------------------------------------------------------

struct Corporal {
  mqtt::InMemoryMockBroker broker;
  station::StationSpec spec;
  std::string controls, indications, plant_topic, lamp;
  std::unique_ptr<station::StationHost> host;
  mqtt::InMemoryMockBroker::Client office{broker};
  uint32_t t = 1000;
  size_t mark = 0;
  bool ok = false;

  explicit Corporal(std::vector<std::string> remote = {}) {
    std::string error;
    ok = station::loadStationFile("build/Corporal.station", spec, error);
    if (!ok) std::printf("    %s\n", error.c_str());
    const std::string base = station::stationTopic(spec.codeline_channel, spec.chart.station);
    controls = base + "/controls";
    indications = base + "/indications";
    plant_topic = base + "/debug/plant";
    lamp = spec.appliance_channel + "/lamp/Corporal:1MC";
    station::HostConfig config;
    config.time_locking_ms = 3000;
    config.remote = std::move(remote);
    host = std::make_unique<station::StationHost>(spec, config, [this](const std::string&) {
      return std::make_unique<mqtt::InMemoryMockBroker::Client>(broker);
    });
    office.connect("office", nullptr);
    if (ok) ok = host->start(error);
    if (!ok) std::printf("    %s\n", error.c_str());
    run(5000);  // every appliance admitted; the startup time locking (3 s here) expires
    mark = broker.log().size();
  }
  void run(uint32_t ms) {
    for (const uint32_t end = t + ms; t < end;) host->tick(t += 10);
  }
  // A complete control frame in chart order: the named AAR tokens asserted, every other dropped.
  std::string frame(std::initializer_list<const char*> asserted) const {
    std::string f = "CHART " + spec.chart.id;
    for (const auto& c : spec.chart.controls) {
      bool on = false;
      for (const char* a : asserted) on = on || c.aar == a;
      f += on ? " " + c.aar : " (" + c.aar + ")";
    }
    return f;
  }
  void code(std::initializer_list<const char*> asserted) { office.publish(controls, frame(asserted), false); }
  void world(const char* appliance, const char* payload) {
    office.publish(spec.simulation_channel + "/" + appliance, payload, false);
  }
  std::string retained(const std::string& topic) const {
    const std::string* r = broker.retained(topic);
    return r ? *r : "<none>";
  }
  // True if the retained indication frame carries exactly this term.
  bool shows(const std::string& term) const {
    std::istringstream in(retained(indications));
    for (std::string w; in >> w;)
      if (w == term) return true;
    return false;
  }
  // Every payload on a topic since the mark, in order, joined by ", ".
  std::string sent(const std::string& topic) const {
    std::string out;
    for (size_t i = mark; i < broker.log().size(); ++i)
      if (broker.log()[i].topic == topic) out += (out.empty() ? "" : ", ") + broker.log()[i].payload;
    return out;
  }
  // Index in the broker's log of the first `payload` on `topic` since the mark; SIZE_MAX if none.
  size_t when(const std::string& topic, const std::string& payload) const {
    for (size_t i = mark; i < broker.log().size(); ++i)
      if (broker.log()[i].topic == topic && broker.log()[i].payload == payload) return i;
    return SIZE_MAX;
  }
  bool plantSays(const std::string& line) const {
    const std::string p = "\n" + retained(plant_topic);
    if (p.find("\n" + line + "\n") != std::string::npos) return true;
    std::printf("    not in the snapshot: %s\n%s", line.c_str(), retained(plant_topic).c_str());
    return false;
  }
};

}  // namespace

// ---- the engine alone ---------------------------------------------------------

SCENARIO("engine: the CALL control lights the lamp, and the CALL indication is the lamp's own report") {
  CallRig r;
  CHECK(!r.bulb.lit());
  CHECK(!r.indicated());
  CHECK(r.code(true) == ControlResult::Applied);
  CHECK(r.cp.maintainerCall("1MC"));  // stored at once
  CHECK(!r.indicated());              // but nothing is lit yet: the indication is not the control
  r.run(100);                          // the scan drives the lamp
  CHECK(r.bulb.lit());
  r.run(100);  // the next scan reads it
  CHECK(r.indicated());
  CHECK(r.code(false) == ControlResult::Applied);  // CALL dropped: a level
  CHECK(r.indicated());                            // still lit until the lamp says otherwise
  r.run(200);
  CHECK(!r.bulb.lit());
  CHECK(!r.indicated());
}

SCENARIO("engine: the call is applied even when the vital part of its frame is refused, and a malformed frame applies nothing") {
  CallRig r;
  r.s1T.shunt(true);  // the OS is occupied: a signal over it is unsafe
  r.run(300);
  CHECK(r.code(true, Word::Normal, Word::Right) == ControlResult::VitalIgnored);
  r.run(300);
  CHECK(r.sig2.active() == Direction::None);  // the vital part did nothing
  CHECK(r.m2R.state().indication == SignalIndication::Stop);
  CHECK(r.bulb.lit());  // the call did
  CHECK(r.indicated());
  engine::ControlTransaction c = r.cp.newControls();
  c.assertFunction(FunctionClass::Switch, "1", Word::Normal);
  c.assertFunction(FunctionClass::Switch, "1", Word::Reverse);  // two words of one group, and no CALL
  CHECK(r.cp.apply(c) == ControlResult::Malformed);
  r.run(300);
  CHECK(r.bulb.lit());  // ignored as a whole: the call stays as it was
  CHECK(r.indicated());
}

SCENARIO("engine: a lamp that is OFFLINE, or restarts, is not indicated; a call still stored relights it when it is admitted again") {
  CallRig r;
  r.code(true);
  r.run(300);
  CHECK(r.indicated());
  r.lamp.offline();  // a maintainer takes the lamp out of service: it stays lit, but nobody vouches for it
  r.run(100);
  CHECK(r.bulb.lit());
  CHECK(!r.indicated());
  const uint32_t drives = r.bulb.driveCount();
  r.run(2000);
  CHECK(r.bulb.driveCount() == drives);  // the engine commands nothing while the lamp is not healthy
  r.lamp.online();                        // restart: dark (Appliance:LAMP Startup State)
  CHECK(!r.bulb.lit());
  r.run(500);  // re-admitted: the stored call is stated again
  CHECK(r.bulb.lit());
  CHECK(r.indicated());
}

SCENARIO("engine: the lamp never touches the vital scan: a route clears and drops the same with the call on, off, or its lamp out of service") {
  auto runMove = [](int mode) {  // 0: no call; 1: call lit; 2: call stored, lamp OFFLINE
    CallRig r;
    if (mode != 0) r.code(true);
    r.run(300);
    if (mode == 2) r.lamp.offline();
    std::string trace;
    auto note = [&](const char* at) {
      trace += std::string(at) + ":" + nameOf(r.m2R.state().indication) + "/" +
               engine::Route::stateName(r.cp.findRoute("MAIN")->state()) + (r.sw.WLR() ? "/free " : "/locked ");
    };
    note("start");
    const ControlResult res = r.code(mode != 0, Word::Normal, Word::Right);
    trace += res == ControlResult::Applied ? "applied " : "refused ";
    r.run(1500);
    note("cleared");
    r.s1T.shunt(true);
    r.run(500);
    note("train");
    r.s1T.shunt(false);
    r.run(500);
    note("gone");
    return trace;
  };
  const std::string plain = runMove(0);
  std::printf("    %s\n", plain.c_str());
  CHECK(plain.find("cleared:CLEAR/Cleared/locked") != std::string::npos);
  CHECK(runMove(1) == plain);
  CHECK(runMove(2) == plain);
}

SCENARIO("engine: a call with no lamp repeats its control, as before; a call added after the first scan is refused and steals no observer") {
  SpikeRig r;  // its 1MC is bound to no lamp
  engine::ControlTransaction c = r.cp.newControls();
  c.assertFunction(FunctionClass::MaintainerCall, "1MC", Word::Call);
  CHECK(r.cp.apply(c) == ControlResult::Applied);
  CHECK(r.cp.indications().is(FunctionClass::MaintainerCall, "1MC", Word::Call));
  SimulatedLampDriver bulb;
  InMemoryNvStore nv;
  LampCore lamp(bulb, nv);
  struct Mine final : ILampObserver {
    int states = 0;
    void onStateChanged(bool) override { ++states; }
    void onEvent(const ApplianceEvent&) override {}
  } mine;
  lamp.setObserver(&mine);
  r.cp.addMaintainerCall("2MC", lamp);  // after the first scan
  CHECK(r.cp.configurationRefused());
  CHECK(r.cp.maintainerCalls().size() == 1);
  lamp.tick(0);
  CHECK(mine.states == 1);  // still the lamp's observer
}

// ---- CP Corporal over MQTT ------------------------------------------------------

SCENARIO("station file: Corporal's call 1MC is bound to lamp Corporal:1MC, and the lamp starts dark and says so") {
  Corporal s;
  CHECK(s.ok);
  CHECK(s.spec.calls.size() == 1);
  CHECK(s.spec.calls[0].name == "1MC" && s.spec.calls[0].role == "lamp" && s.spec.calls[0].id == "Corporal:1MC");
  CHECK(s.lamp == "/layout/SPCoast/appliances/lamp/Corporal:1MC");
  CHECK(s.retained(s.lamp + "/state") == "(ON)");
  CHECK(s.shows("(1MCK)"));
  CHECK(s.plantSays("call 1MC dark"));
}

SCENARIO("1MCS -> lamp/Corporal:1MC/cmd ON -> lamp/Corporal:1MC/state ON -> 1MCK, and back; the answering frame still shows the lamp as it was") {
  Corporal s;
  CHECK(s.ok);
  s.code({"1MCS"});
  CHECK(s.host->field().counts().applied == 1);
  CHECK(s.shows("(1MCK)"));  // the frame that answers the control frame: the lamp has not lit yet
  CHECK(s.plantSays("call 1MC dark, called"));
  s.run(500);
  CHECK(s.sent(s.lamp + "/cmd") == "ON");
  CHECK(s.sent(s.lamp + "/state") == "ON");
  CHECK(s.retained(s.lamp + "/state") == "ON");
  CHECK(s.shows("1MCK"));
  CHECK(s.plantSays("call 1MC lit"));
  // In that order on the wire: the command, the lamp's report, the indication.
  const size_t cmd = s.when(s.lamp + "/cmd", "ON"), state = s.when(s.lamp + "/state", "ON");
  size_t lit = SIZE_MAX;
  for (size_t i = s.mark; i < s.broker.log().size() && lit == SIZE_MAX; ++i) {
    const auto& m = s.broker.log()[i];
    if (m.topic == s.indications && (" " + m.payload + " ").find(" 1MCK ") != std::string::npos) lit = i;
  }
  CHECK(cmd < state && state < lit && lit != SIZE_MAX);
  s.run(3000);
  CHECK(s.sent(s.lamp + "/cmd") == "ON");  // stated once, not every scan

  s.code({});  // CALL dropped
  CHECK(s.shows("1MCK"));  // still lit in the answering frame
  CHECK(s.plantSays("call 1MC lit, not called"));
  s.run(500);
  CHECK(s.sent(s.lamp + "/cmd") == "ON, (ON)");
  CHECK(s.retained(s.lamp + "/state") == "(ON)");
  CHECK(s.shows("(1MCK)"));
  CHECK(s.plantSays("call 1MC dark"));
}

SCENARIO("the lamp is unavailable: (1MCK), whatever was called; powered again it comes back dark, is called again, and 1MCK returns") {
  Corporal s;
  CHECK(s.ok);
  s.code({"1MCS"});
  s.run(500);
  CHECK(s.shows("1MCK"));
  s.world("lamp/Corporal:1MC", "(POWER)");
  s.run(200);
  CHECK(s.retained(s.lamp + "/state") == "UNAVAILABLE");
  CHECK(s.shows("(1MCK)"));
  CHECK(s.plantSays("call 1MC unhealthy (UNAVAILABLE), called"));
  s.mark = s.broker.log().size();
  s.run(3000);
  CHECK(s.sent(s.lamp + "/cmd").empty());  // nothing is commanded to a lamp that is not there
  s.code({"1MCS"});                         // nor does coding the call again light the indication
  s.run(500);
  CHECK(s.shows("(1MCK)"));
  s.world("lamp/Corporal:1MC", "POWER");
  s.run(3000);
  CHECK(s.sent(s.lamp + "/cmd") == "ON");  // re-admitted: the stored call is stated again
  CHECK(s.retained(s.lamp + "/state") == "ON");
  CHECK(s.shows("1MCK"));
  CHECK(s.plantSays("call 1MC lit"));
}

SCENARIO("a lamp unavailable from the start never shows 1MCK and stops nothing else: the station starts and a route clears") {
  Corporal s;
  CHECK(s.ok);
  s.world("lamp/Corporal:1MC", "(POWER)");
  s.run(200);
  s.code({"829NWS", "828WGS", "830WGS", "1MCS"});
  s.run(5000);
  CHECK(s.host->field().counts().applied == 1);
  CHECK(s.shows("830WGK"));
  CHECK(s.shows("828WGK"));
  CHECK(s.shows("(1MCK)"));
}

SCENARIO("the call is applied even when the frame's vital part is refused: the lamp lights, the switch does not move") {
  Corporal s;
  CHECK(s.ok);
  s.world("sensor/829T1", "SHUNT");  // the OS of 829 is occupied: detector locking
  s.run(500);
  CHECK(s.shows("829T1K"));
  s.mark = s.broker.log().size();
  s.code({"829RWS", "1MCS"});
  CHECK(s.host->field().counts().vital_ignored == 1);
  CHECK(s.host->field().counts().applied == 0);
  s.run(1000);
  CHECK(s.sent(s.spec.appliance_channel + "/turnout/829/cmd").empty());  // the vital part did nothing
  CHECK(s.shows("829NWK"));
  CHECK(s.sent(s.lamp + "/cmd") == "ON");  // the call did
  CHECK(s.shows("1MCK"));
}

SCENARIO("a malformed frame applies nothing, the call included; a frame for another chart neither") {
  Corporal s;
  CHECK(s.ok);
  s.office.publish(s.controls, s.frame({"829NWS", "829RWS", "1MCS"}), false);  // two words of one group
  CHECK(s.host->field().counts().malformed == 1);
  std::string other = s.frame({"1MCS"});
  other.replace(6, 8, "00000000");
  s.office.publish(s.controls, other, false);
  CHECK(s.host->field().counts().out_of_sync == 1);
  s.run(1000);
  CHECK(s.sent(s.lamp + "/cmd").empty());
  CHECK(s.shows("(1MCK)"));
}

SCENARIO("a maintainer takes the lamp OFFLINE and cycles it: (1MCK) meanwhile, and the debug channel says why") {
  Corporal s;
  CHECK(s.ok);
  s.code({"1MCS"});
  s.run(500);
  s.office.publish(s.lamp + "/maint", "OFFLINE", false);
  s.run(200);
  CHECK(s.shows("(1MCK)"));
  CHECK(s.plantSays("call 1MC unhealthy (OFFLINE), called"));
  s.office.publish(s.lamp + "/maint", "CYCLE 1 200", false);
  s.run(1000);
  CHECK(s.shows("(1MCK)"));  // the lamp flashing under test is not a call
  s.office.publish(s.lamp + "/maint", "ONLINE", false);
  s.run(3000);
  CHECK(s.retained(s.lamp + "/state") == "ON");
  CHECK(s.shows("1MCK"));
  const std::string debug = station::stationTopic(s.spec.codeline_channel, s.spec.chart.station) + "/debug";
  std::string said;
  for (size_t i = s.mark; i < s.broker.log().size(); ++i)
    if (s.broker.log()[i].topic == debug) said += s.broker.log()[i].payload.substr(s.broker.log()[i].payload.find(' ') + 1) + "\n";
  for (const char* line : {"maintainer call 1MC dark -> lit", "health maintainer call 1MC OK -> OFFLINE",
                           "maintainer call 1MC lit -> unhealthy (OFFLINE)", "health maintainer call 1MC OFFLINE -> OK",
                           "maintainer call 1MC unhealthy (OFFLINE) -> dark"}) {
    const bool found = ("\n" + said).find(std::string("\n") + line + "\n") != std::string::npos;
    if (!found) std::printf("    not said: %s\n%s", line, said.c_str());
    CHECK(found);
  }
}

SCENARIO("the simulation channel takes only POWER and (POWER) for a lamp; a stop of the host leaves lamp/Corporal:1MC/state UNAVAILABLE") {
  Corporal s;
  CHECK(s.ok);
  CHECK(!s.host->simulate("lamp/Corporal:1MC", "ON"));
  CHECK(!s.host->simulate("lamp/Corporal:1MC", "SHUNT"));
  CHECK(s.host->simulate("lamp/Corporal:1MC", "(POWER)"));
  CHECK(s.host->simulate("lamp/Corporal:1MC", "POWER"));
  CHECK(!s.host->simulate("lamp/Corporal:2MC", "(POWER)"));
  s.run(3000);
  CHECK(s.retained(s.lamp + "/state") == "(ON)");
  s.host->shutdown();
  CHECK(s.retained(s.lamp + "/state") == "UNAVAILABLE");
  CHECK(s.retained(s.indications) == "UNAVAILABLE");
}

SCENARIO("--remote lamp/Corporal:1MC: this host simulates no lamp, binds the call to the lamp's topics, and follows whoever hosts it") {
  Corporal s({"lamp/Corporal:1MC"});
  CHECK(s.ok);
  CHECK(s.retained(s.lamp + "/state") == "<none>");  // nobody hosts it yet
  CHECK(s.shows("(1MCK)"));
  CHECK(!s.host->simulate("lamp/Corporal:1MC", "(POWER)"));  // not this host's to power
  // The lamp, hosted elsewhere on the same broker.
  mqtt::InMemoryMockBroker::Client elsewhere(s.broker);
  SimulatedLampDriver bulb;
  InMemoryNvStore nv;
  LampCore lamp(bulb, nv);
  mqtt::MqttLampAdapter adapter(elsewhere, s.spec.appliance_channel, "Corporal:1MC", lamp);
  adapter.connect();
  auto run = [&](uint32_t ms) {
    for (const uint32_t end = s.t + ms; s.t < end;) {
      lamp.tick(s.t + 10);
      adapter.tick(s.t + 10);
      s.host->tick(s.t += 10);
    }
  };
  run(3000);
  s.code({"1MCS"});
  run(500);
  CHECK(bulb.lit());
  CHECK(s.shows("1MCK"));
  std::string error;
  station::HostConfig bad;
  bad.remote = {"lamp/9MC"};
  station::StationHost other(s.spec, bad, [&s](const std::string&) {
    return std::make_unique<mqtt::InMemoryMockBroker::Client>(s.broker);
  });
  CHECK(!other.start(error));  // no such appliance in this station
}

int main() { return codeline::scenario::run(); }
