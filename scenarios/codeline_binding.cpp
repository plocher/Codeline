// The field side of the Code Line MQTT Binding (docs/bindings/CodeLineMQTT.md
// sections 2-5; docs/codeline/README.CodeLine.md 5.3) on CP Corporal, built
// from build/Corporal.station, over InMemoryMockBroker: out of sync,
// malformed, unsafe, retained controls, republish after every control frame,
// retained indications, the will, a clean stop, no frame read before the plant
// runs, and a station file whose route its mast cannot show. Topics are built
// from the station file's channels.
#include <map>
#include <memory>
#include <string>

#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/station/StationHost.h"
#include "scenario.h"

using namespace codeline;
using station::CodeLineField;

namespace {

const char* kSettled =
    "CHART 85973c19 (829NWK) 829RWK (827WLK) (827WLQK) (828WGK) (828EGK) (828TEK) (830WGK) (830EGK) (830TEK) (1MCK) "
    "(827T1K) (829T1K)";
const char* kClear = "CHART 85973c19 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)";

struct Corporal {
  mqtt::InMemoryMockBroker broker;
  station::StationSpec spec;
  std::string controls_topic, indications_topic;
  std::map<std::string, mqtt::InMemoryMockBroker::Client*> clients;
  std::unique_ptr<station::StationHost> host;
  mqtt::InMemoryMockBroker::Client office{broker};
  uint32_t t = 1000;
  bool ok = false;

  explicit Corporal(bool start = true) {
    std::string error;
    ok = station::loadStationFile("build/Corporal.station", spec, error);
    if (!ok) std::printf("    %s\n", error.c_str());
    controls_topic = station::stationTopic(spec.codeline_channel, spec.chart.station) + "/controls";
    indications_topic = station::stationTopic(spec.codeline_channel, spec.chart.station) + "/indications";
    station::HostConfig config;
    config.time_locking_ms = 3000;
    config.initial.emplace_back("turnout/Corporal:829", "REVERSE");
    host = std::make_unique<station::StationHost>(spec, config, [this](const std::string& id) {
      auto c = std::make_unique<mqtt::InMemoryMockBroker::Client>(broker);
      clients[id] = c.get();
      return c;
    });
    office.connect("office", nullptr);
    if (ok && start) boot();
  }
  void boot() {
    std::string error;
    ok = host->start(error);
    if (!ok) std::printf("    %s\n", error.c_str());
    run(5000);  // startup time locking (3 s here) expires
  }
  void run(uint32_t ms) {
    for (const uint32_t end = t + ms; t < end;) host->tick(t += 10);
  }
  size_t published(const std::string& topic) const {
    size_t n = 0;
    for (const auto& m : broker.log()) n += m.topic == topic;
    return n;
  }
  size_t commands(const char* appliance) const {
    const std::string topic = spec.appliance_channel + "/" + appliance + "/cmd";
    size_t n = 0;
    for (const auto& m : broker.log()) n += m.topic == topic;
    return n;
  }
  std::string indications() const {
    const std::string* r = broker.retained(indications_topic);
    return r ? *r : "";
  }
  CodeLineField::Outcome code(const char* frame) {
    const auto before = host->field().counts();
    office.publish(controls_topic, frame, false);
    const auto& c = host->field().counts();
    if (c.applied != before.applied) return CodeLineField::Outcome::Applied;
    if (c.vital_ignored != before.vital_ignored) return CodeLineField::Outcome::VitalIgnored;
    if (c.malformed != before.malformed) return CodeLineField::Outcome::Malformed;
    return CodeLineField::Outcome::OutOfSync;
  }
};

}  // namespace

SCENARIO("a station file's chart is checked against the plant before the field starts") {
  Corporal p(false);
  CHECK(p.ok);
  station::ChartFunction f = p.spec.chart.controls.front();
  f.appliance = "999";
  f.aar = "999NWS";
  p.spec.chart.controls.push_back(f);
  std::string error;
  CHECK(!p.host->start(error));
  CHECK(error.find("999NWS") != std::string::npos);
}

SCENARIO("the indication frame is retained, so an office that starts late lights every lamp at once") {
  Corporal p;
  CHECK(p.indications() == kSettled);
  struct Late final : mqtt::IMqttHandler {
    std::string got;
    bool retained = false;
    void onMessage(std::string_view, std::string_view payload, bool r) override {
      got = std::string(payload);
      retained = r;
    }
  } late;
  mqtt::InMemoryMockBroker::Client c(p.broker);
  c.connect("late-office", nullptr);
  c.subscribe(p.indications_topic, &late);
  CHECK(late.got == kSettled);
  CHECK(late.retained);
}

SCENARIO("rule 4: the field republishes its indications after every control frame, accepted or not") {
  Corporal p;
  const size_t before = p.published(p.indications_topic);
  CHECK(p.code("CHART 85973c19 (829NWS) (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)") ==
        CodeLineField::Outcome::Applied);  // changes nothing
  CHECK(p.published(p.indications_topic) == before + 1);
  CHECK(p.code("CHART 85973c19 829NWS") == CodeLineField::Outcome::Malformed);
  CHECK(p.published(p.indications_topic) == before + 2);
  CHECK(p.code("CHART 6e560295 829NWS") == CodeLineField::Outcome::OutOfSync);
  CHECK(p.published(p.indications_topic) == before + 3);
  CHECK(p.indications() == kSettled);
}

SCENARIO("out of sync: another chart identity is ignored as a whole; the answer carries the field's identity") {
  Corporal p;
  CHECK(p.code("CHART 6e560295 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 1MCS") ==
        CodeLineField::Outcome::OutOfSync);
  p.run(3000);
  CHECK(p.host->field().counts().out_of_sync == 1);
  CHECK(p.commands("turnout/Corporal:829") == 0);
  CHECK(p.indications() == kSettled);  // CHART 85973c19, 829 still reversed, 1MC dark
}

SCENARIO("malformed: missing, extra, repeated, unknown, out-of-order terms or two words of a group change nothing") {
  Corporal p;
  const char* bad[] = {
      "",
      "829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 1MCS",                     // no CHART
      "CHART 85973c19 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) 1MCS",              // missing
      "CHART 85973c19 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 1MCS 1MCS", // extra
      "CHART 85973c19 829NWS (829NWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 1MCS",      // repeated
      "CHART 85973c19 829NWS (829RWS) (827WLS) (828EGS) 828WGS (828HS) 830WGS (830EGS) (830HS) 1MCS",      // order
      "CHART 85973c19 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 2MCS",      // unknown
      "CHART 85973c19 829nws (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 1MCS",      // case
      "CHART 85973c19 (829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 1MCS",     // paren
      "CHART 85973c19 829NWS 829RWS (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 1MCS",        // N and R
      "CHART 85973c19 829NWS (829RWS) (827WLS) 828WGS (828EGS) 828HS 830WGS (830EGS) (830HS) 1MCS",        // L and H
      "CHART 85973c19 829NWS\xC2\xA0(829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 1MCS",  // NBSP
      "CHART 85973c19 829NWS\x0B(829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) 1MCS",     // VT
  };
  for (const char* frame : bad) CHECK(p.code(frame) == CodeLineField::Outcome::Malformed);
  p.run(3000);
  CHECK(p.host->field().counts().malformed == sizeof(bad) / sizeof(bad[0]));
  CHECK(p.commands("turnout/Corporal:829") == 0);
  CHECK(p.indications() == kSettled);  // not even the non-vital 1MCS was applied
}

SCENARIO("separators: any run of ASCII space, tab, CR, LF between terms is read, as tools/codeline_chart.py reads it") {
  Corporal p;
  CHECK(p.code("CHART 85973c19\t(829NWS)  (829RWS)\r\n(827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) 1MCS") ==
        CodeLineField::Outcome::Applied);
  p.run(100);
  CHECK(p.indications().find(" 1MCK ") != std::string::npos);
}

SCENARIO("valid but unsafe: every vital control is ignored, the maintainer call is applied") {
  Corporal p;
  CHECK(p.code(kClear) == CodeLineField::Outcome::Applied);
  p.run(3000);
  const size_t cmds = p.commands("turnout/Corporal:829");
  CHECK(p.code("CHART 85973c19 (829NWS) 829RWS (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) 830HS 1MCS") ==
        CodeLineField::Outcome::VitalIgnored);  // 829 is route locked under 830WAB
  p.run(1000);
  CHECK(p.commands("turnout/Corporal:829") == cmds);
  CHECK(p.indications() ==
        "CHART 85973c19 829NWK (829RWK) (827WLK) (827WLQK) 828WGK (828EGK) (828TEK) 830WGK (830EGK) (830TEK) 1MCK "
        "(827T1K) (829T1K)");
}

SCENARIO("rule 1: a control delivered retained is discarded unread, live or stored before the field starts") {
  Corporal p(false);
  p.office.publish(p.controls_topic, kClear, true);  // a misconfigured office retains a CODE press
  p.boot();
  CHECK(p.host->field().counts().retained_dropped == 1);
  p.office.publish(p.controls_topic, kClear, true);
  p.run(3000);
  CHECK(p.host->field().counts().retained_dropped == 2);
  CHECK(p.host->field().counts().applied == 0);
  CHECK(p.commands("turnout/Corporal:829") == 0);
  CHECK(p.indications() == kSettled);
  p.office.publish(p.controls_topic, "", true);  // clear the stored copy
}

SCENARIO("rule 3: the field's will publishes retained UNAVAILABLE; a clean shutdown says it too") {
  Corporal p;
  p.broker.dropConnection(*p.clients["Corporal-field"]);
  CHECK(p.indications() == "UNAVAILABLE");
  Corporal q;
  q.host->shutdown();
  CHECK(q.indications() == "UNAVAILABLE");
}

SCENARIO("an indication that changes is published without a control frame") {
  Corporal p;
  const size_t before = p.published(p.indications_topic);
  p.office.publish(p.spec.simulation_channel + "/sensor/Corporal:829T1", "SHUNT", false);
  p.run(100);
  CHECK(p.published(p.indications_topic) == before + 1);
  CHECK(p.indications() ==
        "CHART 85973c19 (829NWK) 829RWK (827WLK) (827WLQK) (828WGK) (828EGK) (828TEK) (830WGK) (830EGK) (830TEK) (1MCK) "
        "(827T1K) 829T1K");
}

SCENARIO("a control frame sent before the plant's first scan is never read: nothing counted, nothing commanded") {
  Corporal p(false);
  std::string error;
  CHECK(p.host->start(error));
  p.office.publish(p.controls_topic, "CHART 85973c19 829NWS (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) 1MCS",
                   false);
  const auto& c = p.host->field().counts();
  CHECK(c.applied + c.vital_ignored + c.malformed + c.out_of_sync + c.retained_dropped == 0);
  CHECK(p.published(p.indications_topic) == 0);  // no answer: the field is not on the code line yet
  p.run(5000);
  CHECK(p.commands("turnout/Corporal:829") == 0);
  CHECK(p.indications() == kSettled);
  CHECK(p.code("CHART 85973c19 829NWS (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) 1MCS") ==
        CodeLineField::Outcome::Applied);  // once the plant runs, the same frame is read
  p.run(3000);
  CHECK(p.commands("turnout/Corporal:829") == 1);
}

SCENARIO("a clean stop leaves every appliance's /state and the indications retained UNAVAILABLE, as the wills would") {
  Corporal p;
  p.host->shutdown();
  CHECK(p.indications() == "UNAVAILABLE");
  size_t n = 0;
  auto state = [&p, &n](const std::string& role, const std::string& id) {
    const std::string* r = p.broker.retained(p.spec.appliance_channel + "/" + role + "/" + id + "/state");
    if (r == nullptr || *r != "UNAVAILABLE")
      std::printf("    %s/%s/state reads \"%s\"\n", role.c_str(), id.c_str(), r ? r->c_str() : "<none>");
    n += r != nullptr && *r == "UNAVAILABLE";
  };
  for (const auto& c : p.spec.circuits) state(c.role, c.id);
  for (const auto& s : p.spec.switches) state(s.role, s.id);
  for (const auto& l : p.spec.locks) state(l.role, l.id);
  for (const auto& m : p.spec.masts) state(m.role, m.id);
  CHECK(n == p.spec.circuits.size() + p.spec.switches.size() + p.spec.locks.size() + p.spec.masts.size());
}

SCENARIO("a station file route whose ceiling its mast cannot present is refused before anything starts") {
  Corporal p(false);
  station::RouteSpec r = p.spec.routes.front();  // 828EA-827N-829N
  r.name = "828EA-into-the-siding";
  r.caps.push_back(SignalIndication::Restricting);  // as 827D's IndicationCeiling would give it
  p.spec.routes.push_back(r);
  std::string error;
  CHECK(!p.host->start(error));
  CHECK(error.find("828EA-into-the-siding") != std::string::npos && error.find("RESTRICTING") != std::string::npos);
  std::printf("    refused: %s\n", error.c_str());
}

int main() { return codeline::scenario::run(); }
