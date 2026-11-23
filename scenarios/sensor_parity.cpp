// Appliance:SENSOR functional scenarios, written once against ISensor and run
// twice: direct (SensorCore over SimulatedSensorDriver) and through the MQTT
// loopback (Proxy -> MockBroker -> Adapter -> SensorCore).
// Time moves only through rig.tick(now_ms).
#include "parity.h"

using namespace codeline;
using namespace codeline::testing;
using T = SensorTraits;
using Rec = StateRecorder<T>;

// Boots the rig at `t` with `rec` observing, then forgets the boot traffic.
static void boot(RigOf<T>& rig, Rec& rec, uint32_t t = 0) {
  rig.app().setObserver(&rec);
  rig.tick(t);
  rec.clear();
}

static void withHoldoff(RigOf<T>& rig, uint32_t make_ms, uint32_t dropout_ms) {
  rig.nv().save("holdoff.make_ms", make_ms);
  rig.nv().save("holdoff.dropout_ms", dropout_ms);
}

PARITY_SCENARIO(T, "startup assumes occupied, then clears after dropout_ms of clear") {
  Rec rec;
  rig.app().setObserver(&rec);
  CHECK(rig.app().state() == true);  // unproven before boot: occupied
  rig.tick(0);
  CHECK(rec.timeline == "ACTIVE Config ");  // no OnlineEvent on power-on
  rig.tick(1499);
  CHECK(rig.app().state() == true);
  rig.tick(1500);
  CHECK(rig.app().state() == false);
  CHECK(rec.timeline == "ACTIVE Config (ACTIVE) ");
}

PARITY_SCENARIO(T, "with dropout_ms 0 a clear detector reports vacant at startup") {
  Rec rec;
  withHoldoff(rig, 0, 0);
  rig.app().setObserver(&rec);
  rig.tick(0);
  CHECK(rec.timeline == "(ACTIVE) Config ");
}

PARITY_SCENARIO(T, "an occupied detector at startup reports ACTIVE and stays") {
  Rec rec;
  rig.driver().shunt(true);
  rig.app().setObserver(&rec);
  rig.tick(0);
  rig.tick(5000);
  CHECK(rec.timeline == "ACTIVE Config ");
}

PARITY_SCENARIO(T, "a train shunts at once and a coupler gap shorter than dropout_ms is bridged") {
  Rec rec;
  boot(rig, rec);
  rig.tick(1500);
  rec.clear();
  rig.driver().shunt(true);
  rig.tick(2000);
  CHECK(rig.app().state() == true);  // make_ms 0: immediate
  rig.driver().shunt(false);         // gap between cars, first seen at 2100
  rig.tick(2100);
  rig.driver().shunt(true);  // next car, 900 ms later
  rig.tick(3000);
  rig.driver().shunt(false);  // the train has gone, first seen at 3100
  rig.tick(3100);
  rig.tick(4599);
  CHECK(rig.app().state() == true);
  rig.tick(4600);
  CHECK(rig.app().state() == false);
  CHECK(rec.timeline == "ACTIVE (ACTIVE) ");
}

PARITY_SCENARIO(T, "make_ms filters a shadow shorter than it") {
  Rec rec;
  withHoldoff(rig, 100, 0);
  boot(rig, rec);
  rig.driver().shunt(true);
  rig.tick(10);
  rig.tick(60);
  rig.driver().shunt(false);  // a 50 ms shadow
  rig.tick(70);
  rig.driver().shunt(true);
  rig.tick(200);
  rig.tick(299);
  CHECK(rig.app().state() == false);
  rig.tick(300);
  CHECK(rig.app().state() == true);
  CHECK(rec.timeline == "ACTIVE ");
}

PARITY_SCENARIO(T, "OFFLINE unlocks the holdoff writes, keeps reporting, and ONLINE restarts") {
  Rec rec;
  boot(rig, rec);
  rig.app().setAttribute("holdoff.dropout_ms", SetOp::Assign, AttrValue::number(200));  // ONLINE: ignored
  CHECK(rec.events.empty());
  rig.app().offline();
  rig.app().setAttribute("holdoff.dropout_ms", SetOp::Assign, AttrValue::number(200));
  CHECK(rec.lastValue(EventKind::Config, "holdoff.dropout_ms") == 200);
  rec.clear();
  rig.tick(200);  // the new dropout applies at once
  CHECK(rec.timeline == "(ACTIVE) ");
  rig.driver().shunt(true);  // still reports while OFFLINE (shunt testing)
  rig.tick(210);
  CHECK(rec.timeline == "(ACTIVE) ACTIVE ");
  rig.driver().shunt(false);
  rec.clear();
  rig.app().online();
  CHECK(rec.timeline == "ACTIVE Config Online ");  // restart: occupied again
  rig.tick(409);
  CHECK(rig.app().state() == true);
  rig.tick(410);
  CHECK(rig.app().state() == false);
}

PARITY_SCENARIO(T, "EXPORT CONFIG and EXPORT ATTRIBUTES report every attribute once") {
  Rec rec;
  boot(rig, rec);
  rig.app().exportConfig();
  const Rec::Event* config = rec.last(EventKind::Config);
  CHECK(config && config->data.count == 3);
  CHECK(rec.lastValue(EventKind::Config, "maintenance.post") == 1);
  CHECK(rec.lastValue(EventKind::Config, "holdoff.make_ms") == 0);
  CHECK(rec.lastValue(EventKind::Config, "holdoff.dropout_ms") == 1500);
  rig.app().exportAttributes();
  const Rec::Event* attrs = rec.last(EventKind::Attributes);
  CHECK(attrs && attrs->data.count == 4);
}

PARITY_SCENARIO(T, "cycle is a no-op and identify pulses the beacon") {
  Rec rec;
  boot(rig, rec);
  rig.app().offline();
  rec.clear();
  rig.app().cycle(3, 100);
  rig.tick(1000);
  CHECK(rec.events.empty());
  rig.app().identify(IdentifyMode::Pulse, 300);
  CHECK(rig.driver().beacon());
  rig.tick(1299);
  CHECK(rig.driver().beacon());
  rig.tick(1300);
  CHECK(!rig.driver().beacon());
}

PARITY_SCENARIO(T, "a command before power-on is dropped") {
  Rec rec;
  rig.app().setObserver(&rec);
  rig.app().identify(IdentifyMode::On, 0);
  rig.app().offline();
  rig.tick(0);
  CHECK(!rig.driver().beacon());
  CHECK(rec.count(EventKind::Offline) == 0);
}

PARITY_SCENARIO(T, "bad NV config: CONFIG_ERROR, defaults, and clearFault only once NV is repaired") {
  Rec rec;
  rig.nv().save("holdoff.dropout_ms", 7);  // valid
  rig.nv().save("maintenance.post", 9);    // out of range
  rig.app().setObserver(&rec);
  rig.tick(0);
  CHECK(rec.timeline == "Fault ACTIVE Config ");
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::ConfigError);
  CHECK(rec.lastValue(EventKind::Config, "maintenance.post") == 1);
  CHECK(rec.lastValue(EventKind::Config, "holdoff.dropout_ms") == 7);
  rig.app().offline();
  rig.app().clearFault();  // NV still holds the bad value
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 1);
  rig.app().online();  // re-verified: the same fault again, not counted again
  rig.app().exportAttributes();
  CHECK(rec.count(EventKind::Fault) == 2);
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.fault_count") == 1);
  rig.app().offline();
  rig.app().setAttribute("maintenance.post", SetOp::Assign, AttrValue::boolean(true));
  rig.app().clearFault();
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 0);
}

PARITY_SCENARIO(T, "holdoff timing is right across the uint32 clock wrap and after a long idle") {
  Rec rec;
  boot(rig, rec, 0xFFFFFF00u);
  rig.tick(0xFFFFFF00u + 1500);  // wraps: 1500 ms later
  CHECK(rig.app().state() == false);
  rig.driver().shunt(true);
  rig.tick(2000);
  // Occupied for a long time; the dropout timer is not running meanwhile.
  for (uint32_t i = 1; i < 16; ++i) rig.tick(2000 + (i << 28));
  rig.driver().shunt(false);
  rig.tick(100);
  rig.tick(1599);
  CHECK(rig.app().state() == true);
  rig.tick(1600);
  CHECK(rig.app().state() == false);
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
  maintainerClearsFault(rig, [](ISensor& s) { s.setAttribute("maintenance.post", SetOp::Assign, AttrValue::number(1)); });
}

int main() { return codeline::scenario::run(); }
