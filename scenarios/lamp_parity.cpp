// Appliance:LAMP functional scenarios, written once against ILamp and run
// twice: direct (LampCore over SimulatedLampDriver) and through the MQTT
// loopback (Proxy -> MockBroker -> Adapter -> LampCore). The lamp is the
// maintainer call lamp lamp/Corporal:1MC. Time moves only through rig.tick(now_ms).
// The MQTT-only scenarios (exact topics and payloads, retention, the will) are
// in scenarios/appliance_mqtt.cpp.
#include "parity.h"

using namespace codeline;
using namespace codeline::testing;
using T = LampTraits;
using Rec = StateRecorder<T>;

// Boots the rig at `t` with `rec` observing, then forgets the boot traffic.
static void boot(RigOf<T>& rig, Rec& rec, uint32_t t = 0) {
  rig.app().setObserver(&rec);
  rig.tick(t);
  rec.clear();
}

PARITY_SCENARIO(T, "startup: dark, and it says so") {
  Rec rec;
  rig.app().setObserver(&rec);
  CHECK(rig.app().state() == false);  // unproven before boot: not lit
  rig.tick(0);
  CHECK(rec.timeline == "(ON) Config ");  // no OnlineEvent on power-on
  CHECK(!rig.driver().lit());
  CHECK(rig.app().state() == false);
}

PARITY_SCENARIO(T, "setOn lights and extinguishes the lamp and reports each change once") {
  Rec rec;
  boot(rig, rec);
  rig.app().setOn(true);
  CHECK(rig.driver().lit());
  CHECK(rig.app().state() == true);
  rig.app().setOn(true);  // no change: nothing reported again
  rig.tick(10);
  CHECK(rec.timeline == "ON ");
  rig.app().setOn(false);
  CHECK(!rig.driver().lit());
  CHECK(rig.app().state() == false);
  rig.tick(20);
  CHECK(rec.timeline == "ON (ON) ");
}

PARITY_SCENARIO(T, "a raw /cmd frame is ON or (ON) and nothing else") {
  Rec rec;
  boot(rig, rec);
  rig.sendRawCommand("ON");
  CHECK(rig.driver().lit());
  const uint32_t drives = rig.driver().driveCount();
  for (const char* bad : {"", "on", "ON ON", "ON (ON)", "(ON", "ON)", "((ON))", "OFF", "LIT", "true", "LAMP1"}) {
    rig.sendRawCommand(bad);
    CHECK(rig.driver().lit());
  }
  CHECK(rig.driver().driveCount() == drives);  // a rejected frame never reaches the output
  rig.sendRawCommand("(ON)");
  CHECK(!rig.driver().lit());
  CHECK(rec.timeline == "ON (ON) ");
}

PARITY_SCENARIO(T, "a command before power-on is dropped") {
  Rec rec;
  rig.app().setObserver(&rec);
  rig.app().setOn(true);
  rig.app().identify(IdentifyMode::On, 0);
  rig.tick(0);
  CHECK(!rig.driver().lit());
  CHECK(!rig.driver().beacon());
  CHECK(rig.driver().driveCount() == 1);  // startup extinguishes, once
  CHECK(rec.timeline == "(ON) Config ");
}

PARITY_SCENARIO(T, "onoff.on is the output as an attribute: RW in both modes, and it reads what the lamp is") {
  Rec rec;
  boot(rig, rec);
  rig.app().setAttribute("onoff.on", SetOp::Assign, AttrValue::boolean(true));  // ONLINE: accepted (RW)
  CHECK(rig.driver().lit());
  CHECK(rec.timeline == "ON Attributes ");
  CHECK(rec.lastValue(EventKind::Attributes, "onoff.on") == 1);
  rig.app().setOn(false);
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "onoff.on") == 0);
  rig.app().offline();
  rig.app().setAttribute("onoff.on", SetOp::Assign, AttrValue::boolean(true));  // OFFLINE: accepted too
  CHECK(rig.driver().lit());
  CHECK(rig.app().state() == true);
  rig.app().setAttribute("onoff.on", SetOp::Increment, AttrValue::number(1));  // a bool cannot be nudged
  CHECK(rig.driver().lit());
}

PARITY_SCENARIO(T, "OFFLINE still takes setOn; ONLINE restarts dark") {
  Rec rec;
  boot(rig, rec);
  rig.app().offline();
  rig.app().setOn(true);  // operational: accepted while OFFLINE
  CHECK(rig.driver().lit());
  CHECK(rec.timeline == "Offline ON ");
  rec.clear();
  rig.app().online();
  CHECK(rec.timeline == "(ON) Config Online ");  // restart: dark again, until the controller says otherwise
  CHECK(!rig.driver().lit());
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "onoff.on") == 0);
}

PARITY_SCENARIO(T, "cycle toggles the lamp count times at dwell_ms a step, then restores what was commanded") {
  Rec rec;
  boot(rig, rec);
  rig.app().cycle(2, 100);  // ONLINE: refused
  rig.tick(50);
  CHECK(rec.events.empty() && rec.states.empty());
  rig.app().setOn(true);
  rig.app().offline();
  rig.tick(100);
  rec.clear();
  rig.app().cycle(2, 100);  // at 100: starts lit (it already is: nothing to report)
  rig.tick(199);
  CHECK(rig.driver().lit());
  rig.tick(200);
  CHECK(!rig.driver().lit());
  rig.tick(300);
  CHECK(rig.driver().lit());
  rig.tick(400);
  CHECK(!rig.driver().lit());
  rig.tick(500);  // two passes done: back to the commanded ON
  CHECK(rig.driver().lit());
  rig.tick(1000);
  CHECK(rig.driver().lit());
  CHECK(rec.timeline == "(ON) ON (ON) ON ");
}

PARITY_SCENARIO(T, "setOn ends a cycle, and so does ONLINE") {
  Rec rec;
  boot(rig, rec);
  rig.app().offline();
  rig.app().cycle(50, 100);
  rig.tick(10);
  CHECK(rig.driver().lit());
  rig.app().setOn(false);
  rig.tick(20);
  const uint32_t drives = rig.driver().driveCount();
  rig.tick(2000);
  CHECK(!rig.driver().lit());
  CHECK(rig.driver().driveCount() == drives);
  rig.app().cycle(50, 100);
  rig.tick(2010);
  CHECK(rig.driver().lit());
  rig.app().online();
  rig.tick(2020);
  rig.tick(4000);
  CHECK(!rig.driver().lit());
}

PARITY_SCENARIO(T, "EXPORT CONFIG and EXPORT ATTRIBUTES report every attribute once; identify pulses the beacon") {
  Rec rec;
  boot(rig, rec);
  rig.app().exportConfig();
  const Rec::Event* config = rec.last(EventKind::Config);
  CHECK(config && config->data.count == 1);
  CHECK(rec.lastValue(EventKind::Config, "maintenance.post") == 1);
  rig.app().exportAttributes();
  const Rec::Event* attrs = rec.last(EventKind::Attributes);
  CHECK(attrs && attrs->data.count == 5);
  CHECK(rec.lastValue(EventKind::Attributes, "onoff.on") == 0);
  rig.app().identify(IdentifyMode::Pulse, 300);
  CHECK(rig.driver().beacon());
  rig.tick(299);
  CHECK(rig.driver().beacon());
  rig.tick(300);
  CHECK(!rig.driver().beacon());
  CHECK(!rig.driver().lit());  // the beacon is not the lamp
}

PARITY_SCENARIO(T, "bad NV config: CONFIG_ERROR, defaults, and the lamp still works") {
  Rec rec;
  rig.nv().save("maintenance.post", 9);  // out of range
  rig.app().setObserver(&rec);
  rig.tick(0);
  CHECK(rec.timeline == "Fault (ON) Config ");
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::ConfigError);
  CHECK(rec.lastValue(EventKind::Config, "maintenance.post") == 1);
  rig.app().setOn(true);
  CHECK(rig.driver().lit());
}

PARITY_SCENARIO(T, "cycle timing is right across the uint32 clock wrap") {
  Rec rec;
  boot(rig, rec, 0xFFFFFF00u);
  rig.app().offline();
  rig.app().cycle(1, 1000);
  rig.tick(0xFFFFFF00u + 999);
  CHECK(rig.driver().lit());
  rig.tick(0xFFFFFF00u + 1000);  // wraps
  CHECK(!rig.driver().lit());
}

PARITY_SCENARIO(T, "health: unproven, OK once attributes prove it, OFFLINE, re-proven after ONLINE") {
  HealthBinding<T> b;
  rig.app().setObserver(&b);
  CHECK(b.health.health() == Health::Unproven);
  scan(rig, b, 0);
  CHECK(b.health.ok());
  rig.app().offline();
  CHECK(b.health.health() == Health::Offline);
  rig.app().online();
  CHECK(b.health.health() == Health::Unproven);
  scan(rig, b, 10);
  CHECK(b.health.ok());
}

PARITY_SCENARIO(T, "health after a fault: OFFLINE, repair, CLEAR FAULT, ONLINE ends Ok, never Ok before ONLINE") {
  rig.nv().save("maintenance.post", 9);
  maintainerClearsFault(rig, [](ILamp& l) { l.setAttribute("maintenance.post", SetOp::Assign, AttrValue::number(1)); });
}

int main() { return codeline::scenario::run(); }
