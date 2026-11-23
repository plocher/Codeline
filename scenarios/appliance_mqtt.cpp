// SENSOR, SIGNALMAST and CONTROL scenarios that only exist on the MQTT path:
// exact wire frames (docs/Walkthrough.md, docs/bindings/MQTT.md section 5),
// retention, the last will, the proxy's own link, and bad /state frames.
// Time moves only through rig.tick(now_ms).
#include <string>

#include "codeline/testing/ApplianceRig.h"
#include "codeline/testing/TurnoutRig.h"
#include "scenario.h"

using namespace codeline;
using namespace codeline::testing;
using codeline::mqtt::InMemoryMockBroker;
using SI = SignalIndication;

template <class T>
static bool retainedIs(MqttRigOf<T>& rig, const char* suffix, const char* payload) {
  const std::string* r = rig.retained(suffix);
  return r && *r == payload;
}

// ---- shared transport behaviors, one body per appliance ----

// The appliance drops off the broker: its will replaces /state with a
// retained UNAVAILABLE; the proxy reports its restrictive state, then
// UnavailableEvent; health goes unproven. Reconnecting republishes /state.
template <class T>
static void willScenario(const char* fallback, const char* live) {
  MqttRigOf<T> rig;
  StateRecorder<T> rec;
  rig.app().setObserver(&rec);
  rig.tick(0);
  rig.tick(2000);  // past the sensor's default 1500 ms dropout
  CHECK(retainedIs(rig, "state", live));
  rec.clear();
  rig.broker().dropConnection(rig.applianceClient());
  CHECK(rec.timeline == std::string(fallback) + " Unavailable ");
  CHECK(T::text(rig.app().state()) == fallback);
  CHECK(retainedIs(rig, "state", "UNAVAILABLE"));
  rec.clear();
  rig.adapter().connect();
  CHECK(rec.timeline == std::string(live) + " ");
  CHECK(retainedIs(rig, "state", live));
}

// The office's own link breaks: same fallback and UnavailableEvent, while
// the appliance's retained state is untouched; reconnecting recovers it.
template <class T>
static void linkLossScenario(const char* fallback, const char* live) {
  MqttRigOf<T> rig;
  StateRecorder<T> rec;
  HealthBinding<T> b;
  rig.app().setObserver(&rec);
  rig.tick(0);
  rig.tick(2000);  // past the sensor's default 1500 ms dropout
  rec.clear();
  rig.broker().dropConnection(rig.officeClient());
  CHECK(rec.timeline == std::string(fallback) + " Unavailable ");
  CHECK(retainedIs(rig, "state", live));
  rig.app().setObserver(&b);
  rig.proxy().connect("office");
  CHECK(T::text(rig.app().state()) == live);
  CHECK(b.health.health() == Health::Unproven);  // a state alone proves nothing
  rig.app().exportAttributes();
  CHECK(b.health.ok());
}

// Every /state frame that does not decode leaves the proxy at its fallback,
// never at the last good state, and unproves health: the binding has lost
// sight of the appliance until it proves itself again with attributes.
template <class T>
struct Tee final : T::Observer {
  StateRecorder<T> rec;
  HealthBinding<T> b;
  void onStateChanged(typename T::State s) override {
    rec.onStateChanged(s);
    b.onStateChanged(s);
  }
  void onEvent(const ApplianceEvent& ev) override {
    rec.onEvent(ev);
    b.onEvent(ev);
  }
};

template <class T>
static void badStateScenario(const char* fallback, const char* good, std::initializer_list<const char*> bad) {
  MqttRigOf<T> rig;
  Tee<T> o;
  rig.app().setObserver(&o);
  rig.tick(0);
  InMemoryMockBroker::Client rogue(rig.broker());
  rogue.connect("rogue", nullptr);
  for (const char* payload : bad) {
    rogue.publish(rig.topic("state"), good, false);
    CHECK(T::text(rig.app().state()) == good);
    rig.app().exportAttributes();
    CHECK(o.b.health.ok());
    o.rec.clear();
    rogue.publish(rig.topic("state"), payload, false);
    CHECK(T::text(rig.app().state()) == fallback);
    CHECK(o.rec.timeline == std::string(fallback) + " Unavailable ");
    CHECK(!o.b.health.ok());
    rogue.publish(rig.topic("state"), good, false);  // a good frame alone proves nothing
    CHECK(!o.b.health.ok());
  }
}

// A message on /event that arrives retained is stale (MQTT.md section 4:
// /event is never retained): the proxy drops it.
template <class T>
static void retainedEventScenario() {
  MqttRigOf<T> rig;
  rig.tick(0);
  rig.tick(2000);  // a mast reports its state once STOP is proven
  rig.app().offline();
  InMemoryMockBroker::Client tool(rig.broker());
  tool.connect("tool", nullptr);
  tool.publish(rig.topic("event"),
               "ATTRIBUTES maintenance.offline=false maintenance.id=false maintenance.faulted=false maintenance.fault_count=0",
               true);
  InMemoryMockBroker::Client office2(rig.broker());
  typename T::Proxy late(office2, rig.adapter().applianceTopic());
  HealthBinding<T> b;
  late.setObserver(&b);
  late.connect("office2");  // handed the retained state and the retained event
  CHECK(!b.health.ok());
  CHECK(b.health.health() == Health::Unproven);
  late.exportAttributes();  // the appliance's own answer: OFFLINE
  CHECK(b.health.health() == Health::Offline);
}

// A retained /cmd, /maint or broadcast left by a foreign publisher never acts,
// neither now nor when the appliance reconnects and is handed it.
template <class T, class Untouched>
static void retainedScenario(const char* cmd, Untouched untouched) {
  MqttRigOf<T> rig;
  StateRecorder<T> rec;
  rig.app().setObserver(&rec);
  rig.tick(0);
  rig.tick(2000);  // past the sensor's default 1500 ms dropout
  rec.clear();
  InMemoryMockBroker::Client rogue(rig.broker());
  rogue.connect("rogue", nullptr);
  rogue.publish(rig.topic("cmd"), cmd, true);
  rogue.publish(rig.topic("maint"), "OFFLINE", true);
  rogue.publish(std::string(kChannel) + "/all", "IDENTIFY ON", true);
  CHECK(rig.broker().retained(rig.topic("cmd")) != nullptr);
  rig.applianceClient().disconnect();
  rig.adapter().connect();
  for (uint32_t t = 2010; t <= 4000; t += 10) rig.tick(t);
  CHECK(untouched(rig));
  CHECK(!rig.driver().beacon());
  CHECK(rec.count(EventKind::Offline) == 0);
}

SCENARIO("sensor  a retained /event is stale and never proves health") { retainedEventScenario<SensorTraits>(); }
SCENARIO("mast    a retained /event is stale and never proves health") { retainedEventScenario<MastTraits>(); }
SCENARIO("control a retained /event is stale and never proves health") { retainedEventScenario<ControlTraits>(); }

// ---- TURNOUT (the same transport rules on the older proxy) ----

SCENARIO("turnout a retained /event is stale and never proves health") {
  MqttRig rig;
  rig.tick(0);
  rig.turnout().offline();
  InMemoryMockBroker::Client tool(rig.broker());
  tool.connect("tool", nullptr);
  tool.publish(rig.topic("event"),
               "ATTRIBUTES maintenance.offline=false maintenance.id=false maintenance.faulted=false maintenance.fault_count=0",
               true);
  InMemoryMockBroker::Client office2(rig.broker());
  mqtt::MqttTurnoutProxy late(office2, rig.adapter().applianceTopic());
  struct B final : ITurnoutObserver {
    ApplianceHealth health;
    void onStateChanged(TurnoutState) override { health.onState(); }
    void onEvent(const ApplianceEvent& ev) override { health.onEvent(ev); }
  } b;
  late.setObserver(&b);
  late.connect("office2");
  CHECK(b.health.health() == Health::Unproven);
}

SCENARIO("turnout a bad /state reports IN_TRANSIT, then Unavailable, and unproves health") {
  MqttRig rig;
  Recorder rec;
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  InMemoryMockBroker::Client rogue(rig.broker());
  rogue.connect("rogue", nullptr);
  rec.clear();
  rogue.publish(rig.topic("state"), "NORMAL REVERSE", false);
  CHECK(rec.timeline == "IN_TRANSIT Unavailable ");
}

SCENARIO("turnout and sensor proxies never deliver a ButtonEvent") {
  {
    MqttRig rig;
    Recorder rec;
    rig.turnout().setObserver(&rec);
    rig.tick(0);
    rec.clear();
    rig.officeClient().publish(rig.topic("event"), "BTN1", false);
    rig.officeClient().publish(rig.topic("event"), "(BTN1)", false);
    CHECK(rec.events.empty());
  }
  {
    MqttRigOf<SensorTraits> rig;
    StateRecorder<SensorTraits> rec;
    rig.app().setObserver(&rec);
    rig.tick(0);
    rec.clear();
    rig.officeClient().publish(rig.topic("event"), "BTN2", false);
    CHECK(rec.events.empty());
  }
  {
    MqttRigOf<MastTraits> rig;
    StateRecorder<MastTraits> rec;
    rig.app().setObserver(&rec);
    rig.tick(0);
    rec.clear();
    rig.officeClient().publish(rig.topic("event"), "BTN2", false);
    CHECK(rec.events.empty());
  }
}

// ---- SENSOR ----

SCENARIO("sensor  boot: retained ACTIVE, CONFIG, then (ACTIVE) once the block proves clear (Walkthrough step 1)") {
  MqttRigOf<SensorTraits> rig;
  rig.tick(0);
  CHECK(rig.sent("state", "ACTIVE", true));
  CHECK(rig.sent("event", "CONFIG maintenance.post=true holdoff.make_ms=0 holdoff.dropout_ms=1500", false));
  for (uint32_t t = 10; t <= 5000; t += 10) rig.tick(t);
  CHECK(retainedIs(rig, "state", "(ACTIVE)"));
  CHECK(rig.broker().retained(rig.topic("event")) == nullptr);
  rig.driver().shunt(true);
  rig.tick(5010);
  CHECK(retainedIs(rig, "state", "ACTIVE"));
}

SCENARIO("sensor  has no /cmd: a frame published there changes nothing") {
  MqttRigOf<SensorTraits> rig;
  StateRecorder<SensorTraits> rec;
  rig.app().setObserver(&rec);
  for (uint32_t t = 0; t <= 5000; t += 10) rig.tick(t);
  rec.clear();
  rig.officeClient().publish(rig.topic("cmd"), "ACTIVE", false);
  rig.tick(5010);
  CHECK(rec.timeline.empty());
  CHECK(!rig.app().state());
}

SCENARIO("sensor  the will reports ACTIVE, then Unavailable") { willScenario<SensorTraits>("ACTIVE", "(ACTIVE)"); }
SCENARIO("sensor  the office's own link loss reports ACTIVE, then Unavailable") {
  linkLossScenario<SensorTraits>("ACTIVE", "(ACTIVE)");
}
SCENARIO("sensor  a bad /state is ACTIVE") {
  badStateScenario<SensorTraits>("ACTIVE", "(ACTIVE)", {"", "OCCUPIED", "ACTIVE ACTIVE", "(ACTIVE) X", "UNAVAILABLE"});
}

// ---- SIGNALMAST ----

SCENARIO("mast    boot: /state reads UNAVAILABLE until STOP is proven, then retained STOP; CONFIG first") {
  MqttRigOf<MastTraits> rig;
  rig.nv().save("mast.heads", 2);
  CHECK(retainedIs(rig, "state", "UNAVAILABLE"));  // set on connect: no stale state from an earlier session
  rig.tick(0);
  CHECK(retainedIs(rig, "state", "UNAVAILABLE"));
  CHECK(!rig.sent("state", "STOP", true));
  {  // a controller that joins now learns nothing proven
    InMemoryMockBroker::Client c(rig.broker());
    mqtt::MqttMastProxy late(c, rig.adapter().applianceTopic());
    StateRecorder<MastTraits> rec;
    late.setObserver(&rec);
    late.connect("late");
    CHECK(rec.timeline == "STOP Unavailable ");
  }
  rig.tick(kHeadMoveMs - 1);
  CHECK(retainedIs(rig, "state", "UNAVAILABLE"));
  rig.tick(kHeadMoveMs);
  CHECK(retainedIs(rig, "state", "STOP"));
  int config_at = -1, stop_at = -1, i = 0;
  bool config = false;
  for (const InMemoryMockBroker::Message& m : rig.broker().log()) {
    if (m.topic == rig.topic("event") && m.payload.rfind("CONFIG ", 0) == 0) {
      config_at = i;
      config = m.payload.find("mast.rulebook=SP-1990") != std::string::npos &&
               m.payload.find("mast.heads=2") != std::string::npos &&
               m.payload.find("mast.flash_hz=1.00") != std::string::npos && !m.retained;
    }
    if (m.topic == rig.topic("state") && m.payload == "STOP") stop_at = i;
    ++i;
  }
  CHECK(config);
  CHECK(config_at >= 0 && config_at < stop_at);
}

SCENARIO("mast    830WAB: cmd APPROACH -> state APPROACH (Y over R); cmd STOP -> state STOP only once red is proven (Walkthrough steps 7, cancel)") {
  MqttRigOf<MastTraits> rig;
  rig.nv().save("mast.heads", 2);
  rig.tick(0);
  rig.tick(kHeadMoveMs);
  rig.app().setIndication(SI::Approach, true);  // at t = 300
  CHECK(rig.sent("cmd", "APPROACH", false));
  rig.tick(kHeadMoveMs + 10);
  CHECK(retainedIs(rig, "state", "STOP"));  // the prior confirmed STOP, held
  rig.tick(2 * kHeadMoveMs);
  CHECK(retainedIs(rig, "state", "APPROACH"));
  CHECK(rig.driver().shows(0) == ColorName::Yellow && rig.driver().shows(1) == ColorName::Red);
  rig.app().setIndication(SI::Stop, true);  // at t = 600: head.A proves red at t = 900
  CHECK(rig.sent("cmd", "STOP", false));
  const size_t before = rig.broker().log().size();
  rig.tick(2 * kHeadMoveMs + 10);
  rig.tick(3 * kHeadMoveMs - 1);
  CHECK(retainedIs(rig, "state", "APPROACH"));
  CHECK(rig.broker().log().size() == before);  // nothing at all on /state while head.A turns
  CHECK(rig.app().state().indication == SI::Approach);
  rig.tick(3 * kHeadMoveMs);
  CHECK(retainedIs(rig, "state", "STOP"));
  CHECK(rig.app().state().indication == SI::Stop);
  CHECK(rig.broker().retained(rig.topic("cmd")) == nullptr);
}

SCENARIO("mast    approach lighting on the wire: CLEAR (LIT), LIT, (LIT)") {
  MqttRigOf<MastTraits> rig;
  rig.tick(0);
  rig.app().setIndication(SI::Clear, false);
  CHECK(rig.sent("cmd", "CLEAR (LIT)", false));
  rig.tick(kHeadMoveMs);
  CHECK(retainedIs(rig, "state", "CLEAR (LIT)"));
  rig.app().setLit(true);
  CHECK(rig.sent("cmd", "LIT", false));
  rig.tick(kHeadMoveMs + 1);
  CHECK(retainedIs(rig, "state", "CLEAR"));
  rig.app().setLit(false);
  CHECK(rig.sent("cmd", "(LIT)", false));
  rig.officeClient().publish(rig.topic("cmd"), "APPROACH LIT", false);  // explicit LIT is accepted too
  rig.tick(3 * kHeadMoveMs);
  CHECK(retainedIs(rig, "state", "APPROACH"));
}

SCENARIO("mast    a malformed /cmd is dropped whole") {
  MqttRigOf<MastTraits> rig;
  rig.tick(0);
  rig.tick(kHeadMoveMs);
  const uint32_t sets = rig.driver().setCount(0);
  for (const char* bad : {"", "GREEN", "CLEAR LIT LIT", "LIT CLEAR", "clear", "CLEAR (LIT) X"}) {
    rig.officeClient().publish(rig.topic("cmd"), bad, false);
  }
  CHECK(rig.driver().setCount(0) == sets);
}

SCENARIO("mast    the will reports STOP, then Unavailable") { willScenario<MastTraits>("STOP", "STOP"); }
SCENARIO("mast    the office's own link loss reports STOP, then Unavailable") {
  linkLossScenario<MastTraits>("STOP", "STOP");
}
SCENARIO("mast    a bad /state is STOP") {
  badStateScenario<MastTraits>("STOP", "CLEAR", {"", "GREEN", "CLEAR LIT", "CLEAR (LIT) X", "(CLEAR)", "UNAVAILABLE"});
}
SCENARIO("mast    a retained CLEAR never clears the mast") {
  retainedScenario<MastTraits>("CLEAR", [](MqttRigOf<MastTraits>& r) {
    return r.driver().shows(0) == ColorName::Red && r.core().state().indication == SI::Stop;
  });
}

// ---- CONTROL ----

SCENARIO("control lock:827 wire traffic of MQTT.md 5a and Walkthrough step 1") {
  MqttRigOf<ControlTraits> rig;
  rig.nv().save("lever.outputs", 1);
  rig.nv().save("button.mode", static_cast<uint32_t>(ButtonMode::Level));
  rig.tick(0);
  CHECK(retainedIs(rig, "state", "(SW1)"));  // locked normal
  rig.driver().press(1, true);
  rig.tick(10);
  CHECK(rig.sent("event", "BTN1", false));
  rig.app().setLamps(BitVector{1, 1});
  CHECK(rig.sent("cmd", "LAMP1", false));
  CHECK(rig.driver().lamp(1));
  rig.driver().press(1, false);
  rig.tick(20);
  CHECK(rig.sent("event", "(BTN1)", false));
  rig.driver().setSwitch(1, true);
  rig.tick(30);
  CHECK(retainedIs(rig, "state", "SW1"));
  CHECK(rig.broker().retained(rig.topic("event")) == nullptr);
  CHECK(rig.broker().retained(rig.topic("cmd")) == nullptr);
}

SCENARIO("control CTC column wire traffic of MQTT.md 5b") {
  MqttRigOf<ControlTraits> rig;
  rig.nv().save("lever.inputs", 6);
  rig.nv().save("lever.outputs", 9);
  for (uint8_t k : {1, 4, 5}) rig.driver().setSwitch(k, true);
  rig.tick(0);
  CHECK(retainedIs(rig, "state", "SW1 (SW2) (SW3) SW4 SW5 (SW6)"));
  rig.driver().press(1, true);
  rig.tick(10);
  rig.driver().press(1, false);
  rig.tick(20);
  CHECK(rig.sent("event", "BTN1", false));  // CODE button released
  rig.app().setLamps(BitVector{0x19, 9});
  CHECK(rig.sent("cmd", "LAMP1 (LAMP2) (LAMP3) LAMP4 LAMP5 (LAMP6) (LAMP7) (LAMP8) (LAMP9)", false));
  CHECK(rig.driver().lamps() == 0x19u);
}

SCENARIO("control the proxy ignores malformed button events") {
  MqttRigOf<ControlTraits> rig;
  StateRecorder<ControlTraits> rec;
  rig.app().setObserver(&rec);
  rig.tick(0);
  rec.clear();
  InMemoryMockBroker::Client rogue(rig.broker());
  rogue.connect("rogue", nullptr);
  for (const char* bad : {"BTN", "BTN0", "BTN33", "BTN01", "(BTN)", "BUTTON 1", "BTN1 BTN2"}) {
    rogue.publish(rig.topic("event"), bad, false);
  }
  CHECK(rec.events.empty());
  rogue.publish(rig.topic("event"), "(BTN32)", false);
  CHECK(rec.timeline == "(BTN32) ");
}

SCENARIO("control an IMPORT the core would reject is sent bare, never spelled out of range") {
  MqttRigOf<ControlTraits> rig;
  rig.tick(0);
  rig.app().offline();
  AttrMap m;
  m.add(&kControlAttributes[ControlSchema::table.indexOf("lever.outputs")], AttrValue{1});
  m.add(&kControlAttributes[ControlSchema::table.indexOf("button.mode")], AttrValue{7});
  rig.app().importConfig(m);
  CHECK(rig.sent("maint", "IMPORT", false));
}

SCENARIO("control an IMPORT frame that does not decode is rejected whole with no CONFIG") {
  MqttRigOf<ControlTraits> rig;
  StateRecorder<ControlTraits> rec;
  rig.app().setObserver(&rec);
  rig.tick(0);
  rig.app().offline();
  rec.clear();
  rig.officeClient().publish(rig.topic("maint"), "IMPORT lever.outputs=1 button.inputs=99", false);
  CHECK(rec.count(EventKind::Config) == 0);
  rig.officeClient().publish(rig.topic("maint"), "IMPORT lever.outputs=1", false);
  CHECK(rec.lastValue(EventKind::Config, "lever.outputs") == 1);
}

SCENARIO("control the will reports no levers (count 0), then Unavailable") {
  willScenario<ControlTraits>("-", "(SW1)");
}
SCENARIO("control the office's own link loss reports no levers, then Unavailable") {
  linkLossScenario<ControlTraits>("-", "(SW1)");
}
SCENARIO("control a bad /state proves no lever") {
  badStateScenario<ControlTraits>("-", "(SW1)", {"", "SW2", "(SW1) SW1", "SW1 (SW3)", "SW01", "LAMP1", "UNAVAILABLE"});
}
SCENARIO("control a retained LAMP1 never lights the release lamp") {
  retainedScenario<ControlTraits>("LAMP1", [](MqttRigOf<ControlTraits>& r) { return r.driver().lamps() == 0; });
}

int main() { return codeline::scenario::run(); }
