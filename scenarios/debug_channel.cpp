// The station's debug channel (src/codeline/station/DebugChannel.h) on CP
// Corporal, built from build/Corporal.station, over InMemoryMockBroker:
//
//   {codeline}/Corporal/debug         one unretained line per event, stamped with plant time
//   {codeline}/Corporal/debug/plant   the plant now, retained; UNAVAILABLE by will and on a clean stop
//
// What a control frame came to and why, a signal demand that cleared nothing
// and the hazard, a route's states, the time locking, lock withdrawal and
// travel supervision timers, and an appliance's health. Every hazard named
// here is recorded by the engine where it refused (Plant.h).
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/station/StationHost.h"
#include "scenario.h"

using namespace codeline;
using station::CodeLineField;

namespace {

const char* kClear = "CHART 85973c19 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)";
const char* kCancel = "CHART 85973c19 (829NWS) (829RWS) (827WLS) (828WGS) (828EGS) 828HS (830WGS) (830EGS) 830HS (1MCS)";
const char* kRoute = "route 830WAB-829N-827N";
const char* kNothing = "CHART 85973c19 (829NWS) (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)";

struct Corporal {
  mqtt::InMemoryMockBroker broker;
  station::StationSpec spec;
  std::string controls, debug, plant_topic;
  std::map<std::string, mqtt::InMemoryMockBroker::Client*> clients;
  std::unique_ptr<station::StationHost> host;
  mqtt::InMemoryMockBroker::Client office{broker};
  uint32_t t = 1000;
  size_t mark = 0;  // lines() starts here
  bool ok = false;

  using Initial = std::vector<std::pair<std::string, std::string>>;

  explicit Corporal(uint32_t turnout_travel_ms = 1000, const Initial& initial = {{"turnout/Corporal:829", "REVERSE"}},
                    const char* file = "build/Corporal.station") {
    std::string error;
    ok = station::loadStationFile(file, spec, error);
    if (!ok) std::printf("    %s\n", error.c_str());
    const std::string base = station::stationTopic(spec.codeline_channel, spec.chart.station);
    controls = base + "/controls";
    debug = base + "/debug";
    plant_topic = base + "/debug/plant";
    station::HostConfig config;
    config.time_locking_ms = 3000;
    config.turnout_travel_ms = turnout_travel_ms;
    config.initial = initial;
    host = std::make_unique<station::StationHost>(spec, config, [this](const std::string& id) {
      auto c = std::make_unique<mqtt::InMemoryMockBroker::Client>(broker);
      clients[id] = c.get();
      return c;
    });
    office.connect("office", nullptr);
    if (ok) ok = host->start(error);
    if (!ok) std::printf("    %s\n", error.c_str());
    run(5000);  // startup time locking (3 s here) expires
    mark = broker.log().size();
  }
  void run(uint32_t ms) {
    for (const uint32_t end = t + ms; t < end;) host->tick(t += 10);
  }
  void code(const char* frame) { office.publish(controls, frame, false); }
  size_t commands(const char* appliance) const {
    const std::string topic = spec.appliance_channel + "/" + appliance + "/cmd";
    size_t n = 0;
    for (size_t i = mark; i < broker.log().size(); ++i) n += broker.log()[i].topic == topic;
    return n;
  }
  void world(const char* appliance, const char* payload) {
    office.publish(spec.simulation_channel + "/" + appliance, payload, false);
  }
  // Debug lines since the mark, without their time stamps.
  std::vector<std::string> lines() const {
    std::vector<std::string> out;
    for (size_t i = mark; i < broker.log().size(); ++i) {
      const auto& m = broker.log()[i];
      if (m.topic == debug) out.push_back(m.payload.substr(m.payload.find(' ') + 1));
    }
    return out;
  }
  bool said(const std::string& line) const {
    for (const std::string& l : lines())
      if (l == line) return true;
    std::printf("    not said: %s\n", line.c_str());
    for (const std::string& l : lines()) std::printf("      said: %s\n", l.c_str());
    return false;
  }
  std::string plant() const {
    const std::string* r = broker.retained(plant_topic);
    return r ? *r : "";
  }
  // The snapshot carries exactly this line.
  bool shows(const std::string& line) const {
    const std::string p = "\n" + plant();
    if (p.find("\n" + line + "\n") != std::string::npos) return true;
    std::printf("    not in the snapshot: %s\n%s", line.c_str(), plant().c_str());
    return false;
  }
  size_t snapshots() const {
    size_t n = 0;
    for (size_t i = mark; i < broker.log().size(); ++i) n += broker.log()[i].topic == plant_topic;
    return n;
  }
};

}  // namespace

SCENARIO("a control frame the plant finds unsafe: one line names the outcome and the hazard, an occupied circuit") {
  Corporal p;
  CHECK(p.ok);
  p.world("sensor/Corporal:829T1", "SHUNT");
  p.run(500);
  p.code(kClear);
  CHECK(p.said("track circuit 829T1 clear -> occupied"));
  CHECK(p.said("control frame vital ignored: switch 829 to NORMAL: it is locked by detector locking (829T1 not proven clear)"));
  p.code("CHART 85973c19 (829NWS) (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) 830EGS (830HS) (1MCS)");
  CHECK(p.said("control frame vital ignored: signal 830 RIGHT, route 830EAB-829R: track circuit 829T1 is occupied"));
}

SCENARIO("a control frame applied, malformed and out of sync each say so, with what was wrong") {
  Corporal p;
  p.code("CHART 85973c19 (829NWS) (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) 1MCS");
  CHECK(p.said("control frame applied"));
  p.code("CHART 85973c19 829NWS (829RWS) (827WLS) (828EGS) 828WGS (828HS) 830WGS (830EGS) (830HS) 1MCS");
  CHECK(p.said("control frame malformed: term 4 is (828EGS), the chart has 828WGS there"));
  p.code("CHART 85973c19 829NWS 829RWS (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)");
  CHECK(p.said("control frame malformed: 829RWS is a second asserted word of its positive-control group"));
  p.code("CHART 6e560295 829NWS");
  CHECK(p.said("control frame out of sync: the frame is for chart 6e560295, this field's chart is 85973c19"));
}

SCENARIO("every line is unretained and opens with the plant time of the tick it was said in") {
  Corporal p;
  p.world("sensor/Corporal:832T", "SHUNT");
  p.run(100);
  size_t n = 0;
  for (size_t i = p.mark; i < p.broker.log().size(); ++i) {
    const auto& m = p.broker.log()[i];
    if (m.topic != p.debug) continue;
    ++n;
    CHECK(!m.retained);
    const uint32_t stamp = static_cast<uint32_t>(std::stoul(m.payload));
    CHECK(stamp > 6000 && stamp <= p.t);
  }
  CHECK(n == 1);  // track circuit 832T clear -> occupied
  CHECK(p.broker.retained(p.debug) == nullptr);
}

SCENARIO("a signal demand that clears nothing is dropped, and the line names each route it could have cleared and its hazard") {
  Corporal p;
  // 830 LEFT without 828 LEFT: the frame is safe, 829 goes normal, and then nothing can clear.
  p.code("CHART 85973c19 829NWS (829RWS) (827WLS) (828WGS) (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)");
  p.run(100);
  CHECK(p.shows("lever 830 LEFT (nothing cleared yet)"));
  CHECK(p.shows("switch 829 power IN_TRANSIT moving to NORMAL, travel supervision 2 s remaining free"));
  p.run(2000);
  CHECK(p.said("switch 829 travel supervision started: to NORMAL within 2000 ms"));
  CHECK(p.said("switch 829 reports NORMAL (was IN_TRANSIT)"));
  CHECK(p.said("switch 829 travel supervision ended: it reports NORMAL"));
  CHECK(p.said("signal 830 LEFT demand dropped, nothing cleared: route 830WAB-829N-827N: signal lever 828 is not LEFT"));
  CHECK(p.shows("lever 830 STOP"));
}

SCENARIO("a switch that does not settle: travel supervision expires and the dropped demand says the switch did not settle") {
  Corporal p(5000);  // the points take 5 s; the plant allows 2 s
  p.code(kClear);
  p.run(2500);
  CHECK(p.said("switch 829 travel supervision started: to NORMAL within 2000 ms"));
  CHECK(p.said("switch 829 travel supervision expired: not at NORMAL after 2000 ms (it reports IN_TRANSIT)"));
  for (const char* lever : {"828", "830"})
    CHECK(p.said(std::string("signal ") + lever + " LEFT demand dropped, nothing cleared: route 830WAB-829N-827N: "
                 "switch 829 is not proven NORMAL (it reports IN_TRANSIT, it did not settle within 2000 ms)"));
  CHECK(p.shows("switch 829 power IN_TRANSIT did not reach NORMAL free"));
}

SCENARIO("the plant snapshot is retained, changes when a route clears, and says what holds the route and its switches") {
  Corporal p;
  CHECK(p.shows(std::string(kRoute) + " Idle"));
  CHECK(p.shows("switch 829 power REVERSE free"));
  CHECK(p.shows("switch 827 hand NORMAL locked by lock 827"));
  CHECK(p.shows("lock 827 locked"));
  CHECK(p.shows("lever 830 STOP"));
  CHECK(p.shows("mast 830WAB STOP"));
  CHECK(p.shows("circuit 829T1 clear"));
  const std::string before = p.plant();
  p.run(3000);
  CHECK(p.snapshots() == 0);  // nothing changed: nothing republished
  p.code(kClear);
  p.run(3000);
  CHECK(p.plant() != before);
  CHECK(p.shows(std::string(kRoute) + " Cleared: mast 830WAB demanded APPROACH, shows APPROACH"));
  CHECK(p.shows("switch 829 power NORMAL locked by route 830WAB-829N-827N (cleared), mast 830WAB not proven at STOP"));
  CHECK(p.shows("lever 830 LEFT"));
  CHECK(p.said(std::string(kRoute) + " Idle -> Cleared: signal 830WAB cleared, route locked"));
  struct Late final : mqtt::IMqttHandler {
    std::string got;
    bool retained = false;
    void onMessage(std::string_view, std::string_view payload, bool r) override {
      got = std::string(payload);
      retained = r;
    }
  } late;
  mqtt::InMemoryMockBroker::Client c(p.broker);
  c.connect("late", nullptr);
  c.subscribe(p.plant_topic, &late);
  CHECK(late.retained && late.got == p.plant());
  std::printf("%s", p.plant().c_str());
}

SCENARIO("a cancel under an approaching train: the route's states and the time locking timer, started and expired, and a snapshot once a second") {
  Corporal p;
  p.world("sensor/Corporal:832T", "SHUNT");  // a train in rear of 830WAB
  p.code(kClear);
  p.run(3000);
  p.mark = p.broker.log().size();
  p.code(kCancel);
  p.run(1000);
  CHECK(p.said("control frame applied"));
  CHECK(p.said("signal 830WAB put to STOP, route 830WAB-829N-827N dropped: signal lever 830 is not LEFT"));
  CHECK(p.said(std::string(kRoute) + " Cleared -> Releasing: route locked until mast 830WAB is proven at STOP"));
  CHECK(p.said(std::string(kRoute) + " Releasing -> Timing: time locking started, 3000 ms on signal lever 830 "
                                     "(mast 830WAB proven at STOP, a train may be approaching)"));
  CHECK(p.plant().find(std::string(kRoute) + " Timing: time locking ") != std::string::npos);
  CHECK(p.plant().find("lever 830 STOP, time locking ") != std::string::npos);
  CHECK(p.shows("switch 829 power NORMAL locked by route 830WAB-829N-827N (time locking)"));
  // While it times, a frame that would throw 829 is refused, and says the time locking holds it.
  p.code("CHART 85973c19 (829NWS) 829RWS (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)");
  CHECK(p.said("control frame vital ignored: switch 829 to REVERSE: it is locked by route 830WAB-829N-827N (time locking)"));
  p.run(3000);
  CHECK(p.said(std::string(kRoute) + " Timing -> Idle: time locking expired, route released"));
  CHECK(p.shows(std::string(kRoute) + " Idle"));
  CHECK(p.shows("switch 829 power NORMAL free"));
  std::printf("    %zu snapshots published over the cancel and 3 s of time locking (400 ticks)\n", p.snapshots());
  CHECK(p.snapshots() >= 4 && p.snapshots() <= 10);  // the states, and each whole second of the timer
}

SCENARIO("a train takes the signal: the route traverses and releases, said with the circuit that accepted it") {
  Corporal p;
  p.code(kClear);
  p.run(3000);
  p.world("sensor/Corporal:829T1", "SHUNT");
  p.run(200);
  CHECK(p.said(std::string(kRoute) + " Cleared -> Traversing: a train accepted signal 830WAB (829T1 occupied), sectional release"));
  CHECK(p.shows(std::string(kRoute) + " Traversing: train on the route, holds switches 829,827,827D, until 829T1 clear"));
  p.world("sensor/Corporal:829T1", "(SHUNT)");
  p.run(3000);
  CHECK(p.said(std::string(kRoute) + " Traversing -> Idle: the train is off the route, route released"));
}

SCENARIO("an appliance's health changing is said, and a signal it puts to STOP names the circuit that cannot be read") {
  Corporal p;
  p.code(kClear);
  p.run(3000);
  p.world("sensor/Corporal:827T1", "(POWER)");
  p.run(500);
  CHECK(p.said("health track circuit 827T1 OK -> UNAVAILABLE"));
  CHECK(p.said("signal 830WAB put to STOP, route 830WAB-829N-827N dropped: track circuit 827T1 is not proven clear (unhealthy)"));
  CHECK(p.shows("circuit 827T1 unhealthy (UNAVAILABLE)"));
  p.world("sensor/Corporal:827T1", "POWER");
  p.run(3000);
  CHECK(p.said("health track circuit 827T1 UNAVAILABLE -> OK"));
  CHECK(p.shows("circuit 827T1 clear"));
}

SCENARIO("the electric lock: release granted, a signal over it refused with the reason, and the withdrawal timer starts and expires") {
  Corporal p;
  p.code("CHART 85973c19 (829NWS) (829RWS) 827WLS (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)");
  p.run(500);
  CHECK(p.said("lock 827 locked -> release granted"));
  CHECK(p.shows("switch 827 hand NORMAL free"));
  p.code(kClear);  // the same frame drops the release (a level), but the lock is not proven locked now
  CHECK(p.said("control frame vital ignored: signal 828 LEFT, route 830WAB-829N-827N: lock 827 is not proven locked (release granted)"));
  p.code("CHART 85973c19 (829NWS) (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)");
  p.run(100);
  CHECK(p.said("lock 827 release granted -> withdrawing: lock withdrawal timer started, 1000 ms"));
  CHECK(p.shows("lock 827 withdrawing, proven locked in 1 s"));
  p.run(1500);
  CHECK(p.said("lock 827 withdrawing -> locked: lock withdrawal timer expired, proven locked"));
  CHECK(p.shows("lock 827 locked"));
}

SCENARIO("the hazard named is the one that fails, not the route's first switch or first circuit") {
  Corporal p(1000, {{"turnout/Corporal:829", "NORMAL"}, {"turnout/Corporal:827D", "REVERSE"}});  // the route's third switch is wrong
  p.code(kClear);
  p.run(100);
  CHECK(p.said("signal 830 LEFT demand dropped, nothing cleared: route 830WAB-829N-827N: "
               "switch 827D is not proven NORMAL (it reports REVERSE)"));
  Corporal q;
  q.world("sensor/Corporal:827T1", "SHUNT");  // the route's second circuit
  q.run(500);
  q.code(kClear);
  CHECK(q.said("control frame vital ignored: signal 828 LEFT, route 830WAB-829N-827N: track circuit 827T1 is occupied"));
  Corporal r;
  r.code(kClear);
  r.run(3000);
  r.world("sensor/Corporal:827T1", "SHUNT");  // not the entrance circuit: no train took the signal
  r.run(500);
  CHECK(r.said("signal 830WAB put to STOP, route 830WAB-829N-827N dropped: track circuit 827T1 is occupied"));
}

SCENARIO("a power switch at the wanted end but out of correspondence is said so, not 'not proven REVERSE (it reports REVERSE)'") {
  Corporal p;  // 829 found reversed: that is its demand
  p.world("turnout/Corporal:829", "NORMAL");  // thrown by hand, against the demand
  p.run(500);
  CHECK(p.said("switch 829 reports NORMAL (was REVERSE)"));
  p.code("CHART 85973c19 (829NWS) (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)");
  p.run(100);
  CHECK(p.said("signal 830 LEFT demand dropped, nothing cleared: route 830WAB-829N-827N: "
               "switch 829 reports NORMAL but is out of correspondence: its demand is REVERSE, "
               "which it did not reach within 2000 ms"));
}

SCENARIO("what locks a switch names a route awaiting proof of STOP and a route a train is on") {
  Corporal p;
  p.code(kClear);
  p.run(3000);
  p.code(kCancel);  // no train approaching
  p.run(50);        // the mast has not yet shown STOP
  CHECK(p.shows("switch 829 power NORMAL locked by route 830WAB-829N-827N (awaiting proof of STOP), mast 830WAB not proven at STOP"));
  CHECK(p.shows("lever 830 STOP, time locking awaits proof of STOP"));
  p.run(1000);
  CHECK(p.said(std::string(kRoute) + " Releasing -> Idle: mast 830WAB proven at STOP and no train approaching, route released"));
  p.code(kClear);
  p.run(1000);
  p.world("sensor/Corporal:829T1", "SHUNT");
  p.run(1000);
  CHECK(p.shows("switch 829 power NORMAL locked by detector locking (829T1 not proven clear), route 830WAB-829N-827N (train on the route)"));
  CHECK(p.shows("switch 827 hand NORMAL locked by lock 827, route 830WAB-829N-827N (train on the route)"));
}

SCENARIO("a switch demand the next scan's locks forbid is dropped, and said with what locks it") {
  Corporal p;
  p.world("sensor/Corporal:829T1", "SHUNT");  // a train reaches the OS circuit...
  p.code("CHART 85973c19 829NWS (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)");  // ...before the plant has scanned it
  CHECK(p.said("control frame applied"));  // safe as of the last scan
  p.run(100);
  CHECK(p.said("switch 829 demand NORMAL dropped: locked by detector locking (829T1 not proven clear)"));
  CHECK(p.shows("switch 829 power REVERSE locked by detector locking (829T1 not proven clear)"));
  CHECK(p.commands("turnout/Corporal:829") == 0);
}

SCENARIO("refusals in words: a lock released in the signal's own frame, two signals that conflict, a release under a train, time locking") {
  Corporal p;
  p.code("CHART 85973c19 829NWS (829RWS) 827WLS 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)");
  CHECK(p.said("control frame vital ignored: signal 828 LEFT, route 830WAB-829N-827N: the same frame releases lock 827"));
  p.code("CHART 85973c19 829NWS (829RWS) (827WLS) (828WGS) 828EGS (828HS) 830WGS (830EGS) (830HS) (1MCS)");
  CHECK(p.said("control frame vital ignored: signals of this frame conflict: route 828EA-827N-829N and route 830WAB-829N-827N"));
  // The reason does not stay behind for the next frame.
  p.mark = p.broker.log().size();
  p.code(kNothing);
  CHECK(p.lines() == std::vector<std::string>{"control frame applied"});
  // A train on the route, the section over 827 already released: 827 is free, the route is not idle.
  p.world("sensor/Corporal:832T", "SHUNT");
  p.code(kClear);
  p.run(3000);
  p.world("sensor/Corporal:829T1", "SHUNT");
  p.run(500);
  p.world("sensor/Corporal:827T1", "SHUNT");
  p.run(500);
  p.world("sensor/Corporal:827T1", "(SHUNT)");
  p.run(3000);
  CHECK(p.shows("switch 827 hand NORMAL locked by lock 827"));
  p.code("CHART 85973c19 (829NWS) (829RWS) 827WLS (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)");
  CHECK(p.said("control frame vital ignored: lock 827 release: route 830WAB-829N-827N over switch 827 is Traversing"));
  p.world("sensor/Corporal:829T1", "(SHUNT)");
  p.run(3000);
  // Time locking: clear again, cancel under the approaching train, ask again.
  p.code(kClear);
  p.run(1000);
  p.code(kCancel);
  p.run(400);  // the mast shows STOP after 300 ms: the 3 s have just begun
  CHECK(p.shows("lever 830 STOP, time locking 3 s remaining"));  // the lever that governs the route...
  CHECK(p.shows("lever 828 STOP"));                             // ...not the one it also requires
  p.code(kClear);
  CHECK(p.said("control frame vital ignored: signal 830 LEFT: time locking is running on that lever"));
}

SCENARIO("a control frame's stored demand shows in the snapshot at once, before any scan (as while the clock is paused)") {
  Corporal p;
  CHECK(p.shows("lever 830 STOP"));
  p.code(kClear);  // no tick follows
  CHECK(p.shows("lever 830 LEFT (nothing cleared yet)"));
  CHECK(p.shows("lever 828 LEFT (nothing cleared yet)"));
  CHECK(p.snapshots() == 1);
  p.code(kClear);  // the same again: nothing changed, nothing republished
  CHECK(p.snapshots() == 1);
}

SCENARIO("the lock's request button and a field contact are said when they change") {
  Corporal p;
  p.world("control/Corporal:lock:827", "BTN1");
  p.run(100);
  CHECK(p.said("lock 827 request button pressed"));
  CHECK(p.shows("lock 827 locked, request button held"));
  p.world("control/Corporal:lock:827", "(BTN1)");
  p.run(100);
  CHECK(p.said("lock 827 request button released"));
  CHECK(p.shows("lock 827 locked"));
  Corporal s(1000, {}, "build/Sargent.station");
  CHECK(s.ok);
  CHECK(s.shows("contact HBA open"));
  s.world("sensor/Sargent:HBA", "SHUNT");
  s.run(500);
  CHECK(s.said("contact HBA open -> closed"));
  CHECK(s.shows("contact HBA closed"));
}

SCENARIO("the debug connection is lost: the station runs on, the will says UNAVAILABLE, and the channel connects and publishes the snapshot again") {
  Corporal p;
  CHECK(p.plant() != "UNAVAILABLE" && !p.plant().empty());
  p.broker.dropConnection(*p.clients["Corporal-debug"]);
  CHECK(p.plant() == "UNAVAILABLE");
  p.code(kClear);  // the station is not held up: the route clears
  p.run(900);
  CHECK(p.plant() == "UNAVAILABLE");
  p.run(2100);  // the first retry is due 1 to 1.25 s after the loss
  CHECK(p.host->field().counts().applied == 1);
  CHECK(p.clients["Corporal-debug"]->connected());
  CHECK(p.shows(std::string(kRoute) + " Cleared: mast 830WAB demanded APPROACH, shows APPROACH"));
  p.world("sensor/Corporal:832T", "SHUNT");
  p.run(100);
  CHECK(p.said("track circuit 832T clear -> occupied"));  // and it speaks again
}

SCENARIO("a stop of the host leaves the plant snapshot retained UNAVAILABLE, and the channel does not connect again") {
  Corporal q;
  q.host->shutdown();
  CHECK(q.plant() == "UNAVAILABLE");
}

int main() { return codeline::scenario::run(); }
