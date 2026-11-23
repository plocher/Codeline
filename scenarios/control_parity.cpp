// Appliance:CONTROL functional scenarios, written once against IControl and
// run twice: direct (ControlCore over SimulatedControlDriver) and through the
// MQTT loopback (Proxy -> MockBroker -> Adapter -> ControlCore).
// Profiles from docs/bindings/MQTT.md section 5: the electric lock lock:827
// (5a) and the CTC panel column (5b). Time moves only through rig.tick().
#include "parity.h"

using namespace codeline;
using namespace codeline::testing;
using T = ControlTraits;
using Rec = StateRecorder<T>;

static BitVector frame(uint32_t bits, uint8_t count) { return BitVector{bits, count}; }

// lock:827 (MQTT.md 5a): 1 MAINTAINED input, 1 output, 1 LEVEL button.
static void lockProfile(RigOf<T>& rig) {
  rig.nv().save("lever.inputs", 1);
  rig.nv().save("lever.outputs", 1);
  rig.nv().save("button.inputs", 1);
  rig.nv().save("button.mode", static_cast<uint32_t>(ButtonMode::Level));
}

// The Corporal CTC column (MQTT.md 5b): 6 levers, 9 lamps, 1 ONESHOT_RELEASE CODE button.
static void columnProfile(RigOf<T>& rig) {
  rig.nv().save("lever.inputs", 6);
  rig.nv().save("lever.outputs", 9);
  rig.nv().save("button.inputs", 1);
  rig.nv().save("button.mode", static_cast<uint32_t>(ButtonMode::OneshotRelease));
}

static void boot(RigOf<T>& rig, Rec& rec) {
  rig.app().setObserver(&rec);
  rig.tick(0);
  rec.clear();
}

PARITY_SCENARIO(T, "lock:827 powers up reporting (SW1) with the release lamp dark") {
  Rec rec;
  lockProfile(rig);
  rig.driver().press(1, false);
  rig.app().setObserver(&rec);
  rig.tick(0);
  CHECK(rec.timeline == "(SW1) Config ");
  CHECK(rec.lastValue(EventKind::Config, "lever.outputs") == 1);
  CHECK(rec.lastValue(EventKind::Config, "button.mode") == static_cast<uint32_t>(ButtonMode::Level));
  CHECK(rig.driver().lamps() == 0);
  CHECK(rig.app().state() == frame(0, 1));
}

PARITY_SCENARIO(T, "lock:827 unlock sequence of MQTT.md 5a: BTN1, LAMP1, (BTN1), SW1") {
  Rec rec;
  lockProfile(rig);
  boot(rig, rec);
  rig.driver().press(1, true);  // crew presses and holds the unlock request
  rig.tick(10);
  rig.app().setLamps(frame(1, 1));  // controller lights the release indicator
  CHECK(rig.driver().lamp(1));
  rig.driver().press(1, false);  // crew releases the button
  rig.tick(20);
  rig.driver().setSwitch(1, true);  // crew opens the lock
  rig.tick(30);
  CHECK(rec.timeline == "BTN1 (BTN1) SW1 ");
  CHECK(rig.app().state() == frame(1, 1));
  rig.tick(40);
  CHECK(rec.timeline == "BTN1 (BTN1) SW1 ");  // nothing repeats while nothing changes
}

PARITY_SCENARIO(T, "ONESHOT_RELEASE reports one BTN<k> on release and nothing on press") {
  Rec rec;
  columnProfile(rig);
  boot(rig, rec);
  rig.driver().press(1, true);
  rig.tick(10);
  CHECK(rec.events.empty());
  rig.driver().press(1, false);
  rig.tick(20);
  rig.tick(30);
  CHECK(rec.timeline == "BTN1 ");
  CHECK(rec.last(EventKind::Button)->channel == 1);
  CHECK(rec.last(EventKind::Button)->pressed);
}

PARITY_SCENARIO(T, "the CTC column reports all six levers as one frame and takes nine lamps at once") {
  Rec rec;
  columnProfile(rig);
  for (uint8_t k : {1, 4, 5}) rig.driver().setSwitch(k, true);
  rig.app().setObserver(&rec);
  rig.tick(0);
  CHECK(rec.timeline == "SW1 (SW2) (SW3) SW4 SW5 (SW6) Config ");
  rig.sendRawCommand("LAMP1 (LAMP2) (LAMP3) LAMP4 LAMP5 (LAMP6) (LAMP7) (LAMP8) (LAMP9)");
  CHECK(rig.driver().lamps() == 0x19u);
  rig.driver().setSwitch(2, true);
  rig.driver().setSwitch(4, false);
  rig.tick(10);  // two levers in one sample: one frame
  CHECK(rec.timeline == "SW1 (SW2) (SW3) SW4 SW5 (SW6) Config SW1 SW2 (SW3) (SW4) SW5 (SW6) ");
}

PARITY_SCENARIO(T, "a lamp frame whose count is not lever.outputs is rejected whole") {
  Rec rec;
  lockProfile(rig);
  boot(rig, rec);
  rig.app().setLamps(frame(1, 1));  // release lamp lit; every bad frame below must leave it lit
  CHECK(rig.driver().lamps() == 1);
  const uint32_t drives = rig.driver().driveCount();
  rig.app().setLamps(frame(2, 2));
  CHECK(rig.driver().lamps() == 1);
  rig.app().setLamps(frame(0, 0));
  CHECK(rig.driver().lamps() == 1);
  for (const char* bad : {"LAMP1 (LAMP2)", "(LAMP1) (LAMP2)", "", "(LAMP2)", "LAMP01", "SW1"}) {
    rig.sendRawCommand(bad);
    CHECK(rig.driver().lamps() == 1);
  }
  CHECK(rig.driver().driveCount() == drives);
  rig.sendRawCommand("(LAMP1)");
  CHECK(rig.driver().lamps() == 0);
  rig.sendRawCommand("LAMP1");
  CHECK(rig.driver().lamps() == 1);
  CHECK(rec.events.empty());
}

PARITY_SCENARIO(T, "changing lever.outputs OFFLINE extinguishes every lamp") {
  Rec rec;
  rig.nv().save("lever.outputs", 3);
  boot(rig, rec);
  rig.app().setLamps(frame(0x7, 3));
  rig.app().offline();
  rig.app().setAttribute("lever.outputs", SetOp::Assign, AttrValue::number(2));
  CHECK(rig.driver().lamps() == 0);
  rig.tick(10);
  CHECK(rig.driver().lamps() == 0);
  rig.app().setLamps(frame(0x3, 2));
  CHECK(rig.driver().lamps() == 0x3);
}

PARITY_SCENARIO(T, "IMPORT is all-or-nothing: one bad entry changes nothing and emits no CONFIG") {
  Rec rec;
  boot(rig, rec);
  rig.app().offline();
  rec.clear();
  const AttributeInfo* outputs = &kControlAttributes[ControlSchema::table.indexOf("lever.outputs")];
  const AttributeInfo* inputs = &kControlAttributes[ControlSchema::table.indexOf("button.inputs")];
  const AttributeInfo* mode = &kControlAttributes[ControlSchema::table.indexOf("button.mode")];
  const AttributeInfo* offline = &kControlAttributes[ControlSchema::table.indexOf("maintenance.offline")];
  AttrMap bad_count, bad_enum, not_config;
  bad_count.add(outputs, AttrValue{1});
  bad_count.add(inputs, AttrValue{99});
  bad_enum.add(outputs, AttrValue{1});
  bad_enum.add(mode, AttrValue{7});  // past the last ButtonMode
  not_config.add(outputs, AttrValue{1});
  not_config.add(offline, AttrValue{0});  // volatile: not importable
  for (const AttrMap* m : {&bad_count, &bad_enum, &not_config}) {
    rec.clear();
    rig.app().importConfig(*m);
    CHECK(rec.count(EventKind::Config) == 0);
  }
  uint32_t v = 0;
  CHECK(!rig.nv().load("lever.outputs", v));
  AttrMap good;
  good.add(outputs, AttrValue{1});
  good.add(mode, AttrValue{static_cast<uint32_t>(ButtonMode::Level)});
  rig.app().importConfig(good);
  CHECK(rec.lastValue(EventKind::Config, "lever.outputs") == 1);
  CHECK(rec.lastValue(EventKind::Config, "button.mode") == static_cast<uint32_t>(ButtonMode::Level));
  CHECK(rig.nv().load("lever.outputs", v) && v == 1);
}

PARITY_SCENARIO(T, "a command before power-on is dropped and never reaches the lamps") {
  Rec rec;
  lockProfile(rig);
  rig.app().setObserver(&rec);
  rig.app().setLamps(frame(1, 1));
  rig.app().identify(IdentifyMode::On, 0);
  CHECK(rig.driver().driveCount() == 0);
  CHECK(rig.app().state().count == 0);  // proves no lever position yet
  rig.tick(0);
  CHECK(rig.driver().lamps() == 0);
  CHECK(!rig.driver().beacon());
}

PARITY_SCENARIO(T, "online() with the release lamp lit and the button held: lamp dark, no button event") {
  Rec rec;
  lockProfile(rig);
  boot(rig, rec);
  rig.app().setLamps(frame(1, 1));
  rig.app().offline();
  rig.driver().press(1, true);
  rig.tick(10);
  rec.clear();
  rig.app().online();
  CHECK(rig.driver().lamps() == 0);
  rig.tick(20);
  CHECK(rec.timeline == "(SW1) Config Online ");
  rig.driver().press(1, false);
  rig.tick(30);
  CHECK(rec.timeline == "(SW1) Config Online (BTN1) ");  // the next change is reported
}

PARITY_SCENARIO(T, "cycle walks lamp 1..M OFFLINE, then restores the last frame; ignored ONLINE") {
  Rec rec;
  rig.nv().save("lever.outputs", 3);
  boot(rig, rec);
  rig.app().setLamps(frame(0x5, 3));
  rig.app().cycle(1, 100);
  rig.tick(1000);
  CHECK(rig.driver().lamps() == 0x5);
  rig.app().offline();
  const uint32_t t0 = 0xFFFFFFA0u;  // the walk crosses the uint32 wrap
  rig.tick(t0);
  rig.app().cycle(2, 100);
  uint32_t seen[8] = {};
  int n = 0;
  uint32_t last = rig.driver().lamps();
  seen[n++] = last;
  for (uint32_t dt = 10; dt <= 700; dt += 10) {
    rig.tick(t0 + dt);
    if (rig.driver().lamps() != last && n < 8) seen[n++] = last = rig.driver().lamps();
  }
  CHECK(n == 7);
  CHECK(seen[0] == 1 && seen[1] == 2 && seen[2] == 4 && seen[3] == 1 && seen[4] == 2 && seen[5] == 4);
  CHECK(seen[6] == 0x5);  // restored
}

PARITY_SCENARIO(T, "setLamps ends a cycle and wins") {
  Rec rec;
  rig.nv().save("lever.outputs", 3);
  boot(rig, rec);
  rig.app().offline();
  rig.app().cycle(5, 100);
  rig.tick(150);
  rig.app().setLamps(frame(0x6, 3));
  for (uint32_t t = 160; t <= 2000; t += 10) rig.tick(t);
  CHECK(rig.driver().lamps() == 0x6);
}

PARITY_SCENARIO(T, "lever and button channel counts are VITAL and stop at 32") {
  Rec rec;
  boot(rig, rec);
  rig.app().setAttribute("lever.outputs", SetOp::Assign, AttrValue::number(2));  // ONLINE: ignored
  CHECK(rec.events.empty());
  rig.app().offline();
  rig.app().setAttribute("lever.inputs", SetOp::Assign, AttrValue::number(33));
  rig.app().setAttribute("lever.inputs", SetOp::Assign, AttrValue::number(0));
  rig.app().setAttribute("lever.outputs", SetOp::Assign, AttrValue::number(33));
  rig.app().setAttribute("button.inputs", SetOp::Increment, AttrValue::number(32));
  CHECK(rec.count(EventKind::Config) == 0);
  rig.app().setAttribute("lever.inputs", SetOp::Assign, AttrValue::number(32));
  CHECK(rec.lastValue(EventKind::Config, "lever.inputs") == 32);
  AttrMap import;
  import.add(&kControlAttributes[ControlSchema::table.indexOf("lever.outputs")], AttrValue{33});
  rig.app().importConfig(import);
  CHECK(rec.lastValue(EventKind::Config, "lever.outputs") == 0);
}

PARITY_SCENARIO(T, "an NV channel count above 32 raises CONFIG_ERROR once and clears only after repair") {
  Rec rec;
  rig.nv().save("lever.outputs", 40);
  rig.app().setObserver(&rec);
  rig.tick(0);
  CHECK(rec.timeline == "Fault (SW1) Config ");
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::ConfigError);
  CHECK(rec.lastValue(EventKind::Config, "lever.outputs") == 0);
  rig.app().offline();
  rig.app().online();  // the same fault found again: not a new one
  rig.app().offline();
  rig.app().clearFault();
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 1);
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.fault_count") == 1);
  rig.app().setAttribute("lever.outputs", SetOp::Assign, AttrValue::number(1));  // rewrites NV
  rig.app().clearFault();
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 0);
  CHECK(rec.count(EventKind::Fault) == 2);  // reported again by the second restart, counted once
}

PARITY_SCENARIO(T, "identify pulses the beacon and never touches the lamps") {
  Rec rec;
  lockProfile(rig);
  boot(rig, rec);
  const uint32_t drives = rig.driver().driveCount();
  rig.app().identify(IdentifyMode::Pulse, 500);
  CHECK(rig.driver().beacon());
  rig.tick(499);
  CHECK(rig.driver().beacon());
  rig.tick(500);
  CHECK(!rig.driver().beacon());
  CHECK(rig.driver().driveCount() == drives);
}

PARITY_SCENARIO(T, "health: unproven, OK once attributes prove it, faulted, re-proven after ONLINE") {
  HealthBinding<T> b;
  rig.nv().save("button.inputs", 99);
  rig.app().setObserver(&b);
  scan(rig, b, 0);
  CHECK(b.health.health() == Health::Faulted);
  rig.app().offline();
  rig.app().setAttribute("button.inputs", SetOp::Assign, AttrValue::number(1));
  rig.app().clearFault();
  rig.app().online();
  CHECK(b.health.health() == Health::Unproven);
  scan(rig, b, 10);
  CHECK(b.health.ok());
  scan(rig, b, 500);
  CHECK(b.health.ok());
}

PARITY_SCENARIO(T, "health after a fault: OFFLINE, repair, CLEAR FAULT, ONLINE ends Ok, never Ok before ONLINE") {
  rig.nv().save("lever.outputs", 40);
  maintainerClearsFault(rig, [](IControl& c) { c.setAttribute("lever.outputs", SetOp::Assign, AttrValue::number(1)); });
}

int main() { return codeline::scenario::run(); }
