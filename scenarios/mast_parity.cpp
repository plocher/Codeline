// Appliance:SIGNALMAST functional scenarios, written once against IMast and
// run twice: direct (MastCore over SimulatedMastDriver) and through the MQTT
// loopback (Proxy -> MockBroker -> Adapter -> MastCore).
// Time moves only through rig.tick(now_ms). Heads are searchlights whose
// mechanism takes kHeadMoveMs to change color.
#include "parity.h"

using namespace codeline;
using namespace codeline::testing;
using T = MastTraits;
using Rec = StateRecorder<T>;
using SI = SignalIndication;

static constexpr uint32_t kMove = kHeadMoveMs;

// Boots the rig at t=0 with `rec` observing, lets the heads settle at STOP,
// then forgets the boot traffic.
static void boot(RigOf<T>& rig, Rec& rec, uint8_t heads = 1) {
  if (heads != 1) rig.nv().save("mast.heads", heads);
  rig.app().setObserver(&rec);
  rig.tick(0);
  rig.tick(kMove);
  rec.clear();
}

static bool shows(RigOf<T>& rig, ColorName a, ColorName b = ColorName::None, ColorName c = ColorName::None) {
  return rig.driver().shows(0) == a && rig.driver().shows(1) == b && rig.driver().shows(2) == c;
}

PARITY_SCENARIO(T, "power-on drives STOP and reports it only once every head proves red") {
  Rec rec;
  rig.nv().save("mast.heads", 2);
  rig.app().setObserver(&rec);
  CHECK(rig.app().state() == (MastState{SI::Stop, true}));  // the Startup State, unproven
  rig.tick(0);
  CHECK(rec.timeline == "Config ");  // CONFIG first; the state waits for proof
  CHECK(shows(rig, ColorName::Red, ColorName::Red));
  CHECK(rig.driver().lit());
  rig.tick(kMove - 1);
  CHECK(rec.timeline == "Config ");
  rig.tick(kMove);
  CHECK(rec.timeline == "Config STOP ");
  rig.tick(2 * kMove);
  CHECK(rec.timeline == "Config STOP ");
}

PARITY_SCENARIO(T, "power-on with a head that never proves: no state is ever reported, health never Ok") {
  HealthBinding<T> b;
  rig.driver().stick(0, true);
  rig.app().setObserver(&b);
  bool ok = false;
  for (uint32_t t = 0; t <= 5000; t += 50) {
    scan(rig, b, t);
    ok |= b.health.ok();
  }
  CHECK(!ok);
  CHECK(b.health.health() == Health::Unproven);
}
PARITY_SCENARIO(T, "APPROACH on two heads: STOP held while head.A turns, then APPROACH (Walkthrough step 7)") {
  Rec rec;
  boot(rig, rec, 2);
  rig.app().setIndication(SI::Approach, true);
  CHECK(shows(rig, ColorName::Yellow, ColorName::Red));  // Yellow over Red
  rig.tick(kMove + 10);
  CHECK(rig.app().state().indication == SI::Stop);  // prior confirmed indication held
  rig.tick(2 * kMove - 1);
  CHECK(rig.app().state().indication == SI::Stop);
  CHECK(rec.timeline.empty());
  rig.tick(2 * kMove);
  CHECK(rig.app().state() == (MastState{SI::Approach, true}));
  CHECK(rec.timeline == "APPROACH ");
}
PARITY_SCENARIO(T, "a cancel holds CLEAR while head.A turns and reports STOP only once red is proven") {
  Rec rec;
  boot(rig, rec, 2);
  rig.app().setIndication(SI::Clear, true);
  rig.tick(2 * kMove);
  CHECK(rig.app().state().indication == SI::Clear);
  rig.app().setIndication(SI::Stop, true);  // at t = 600; head.A proves red at t = 900
  CHECK(shows(rig, ColorName::Red, ColorName::Red));
  rig.tick(2 * kMove + 1);
  CHECK(rig.app().state().indication == SI::Clear);  // not yet: the head is still turning
  rig.tick(3 * kMove - 1);
  CHECK(rig.app().state().indication == SI::Clear);
  CHECK(rec.timeline == "CLEAR ");
  rig.tick(3 * kMove);
  CHECK(rig.app().state().indication == SI::Stop);
  CHECK(rec.timeline == "CLEAR STOP ");
}
PARITY_SCENARIO(T, "an indication the rulebook does not cover shows and reports STOP") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().setIndication(SI::DivergingClear, true);  // two-head only
  rig.tick(3 * kMove);
  CHECK(shows(rig, ColorName::Red));
  rig.app().setIndication(SI::Restricting, true);  // not in SP-1990 here
  rig.tick(6 * kMove);
  CHECK(rig.app().state().indication == SI::Stop);
  CHECK(rec.timeline.empty());
}

PARITY_SCENARIO(T, "two-head diverging aspects; proceed to proceed holds the old indication while head.B turns") {
  Rec rec;
  boot(rig, rec, 2);
  rig.app().setIndication(SI::DivergingApproach, true);
  CHECK(shows(rig, ColorName::Red, ColorName::Yellow));
  rig.tick(2 * kMove);
  rig.app().setIndication(SI::DivergingClear, true);
  CHECK(shows(rig, ColorName::Red, ColorName::Green));
  rig.tick(2 * kMove + 1);  // head.B turning from Yellow to Green
  CHECK(rig.app().state().indication == SI::DivergingApproach);
  rig.tick(4 * kMove);
  CHECK(rec.timeline == "DIVERGING_APPROACH DIVERGING_CLEAR ");
}

PARITY_SCENARIO(T, "CLEAR to APPROACH holds CLEAR until Yellow is proven, never passing through STOP") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().setIndication(SI::Clear, true);
  rig.tick(2 * kMove);
  rig.app().setIndication(SI::Approach, true);
  for (uint32_t t = 2 * kMove + 10; t < 3 * kMove; t += 10) {
    rig.tick(t);
    CHECK(rig.app().state().indication == SI::Clear);
  }
  rig.tick(3 * kMove);
  CHECK(rec.timeline == "CLEAR APPROACH ");
}
PARITY_SCENARIO(T, "a failed head raises HW_ERROR once and never yields a STOP report") {
  Rec rec;
  boot(rig, rec, 2);
  rig.app().setIndication(SI::Clear, true);
  rig.tick(2 * kMove);
  rig.driver().fail(1, true);  // head.B reported failed by its driver
  rig.tick(2 * kMove + 1);
  CHECK(rec.timeline == "CLEAR Fault ");
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::HwError);
  rig.app().setIndication(SI::Stop, true);  // the cancel cannot be proven
  for (uint32_t t = 2 * kMove + 10; t <= 10 * kMove; t += 10) rig.tick(t);
  CHECK(rig.app().state().indication == SI::Clear);  // held: never a STOP for a failure
  CHECK(rec.timeline == "CLEAR Fault ");
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 1);
  rig.app().offline();
  rig.app().clearFault();  // still failed: stays faulted
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 1);
  rig.app().online();  // a restart re-verifies: the failure is reported again, not counted again
  rig.tick(10 * kMove + 5);
  rig.app().exportAttributes();
  CHECK(rec.count(EventKind::Fault) == 2);
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.fault_count") == 1);
  rig.app().offline();
  rig.driver().fail(1, false);  // repaired
  rig.tick(10 * kMove + 10);
  CHECK(rec.states.back().indication == SI::Stop);  // now proven
  rig.app().clearFault();
  rig.app().online();
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 0);
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.fault_count") == 1);
  rig.driver().fail(0, true);  // a new failure episode raises again
  rig.tick(10 * kMove + 20);
  CHECK(rec.count(EventKind::Fault) == 3);
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::HwError);
}

PARITY_SCENARIO(T, "each failure episode raises one HW_ERROR, counted once while faulted") {
  Rec rec;
  boot(rig, rec, 1);
  rig.driver().fail(0, true);
  rig.tick(kMove + 10);
  rig.tick(kMove + 20);
  rig.driver().fail(0, false);  // the driver sees it recover
  rig.tick(kMove + 30);
  rig.driver().fail(0, true);  // and fail again
  rig.tick(kMove + 40);
  CHECK(rec.timeline == "Fault Fault ");
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.fault_count") == 1);  // still faulted in between
}

PARITY_SCENARIO(T, "a head that never proves and is not reported failed holds the prior indication forever") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().setIndication(SI::Clear, true);
  rig.tick(2 * kMove);
  rig.driver().stick(0, true);
  rig.app().setIndication(SI::Stop, true);
  for (uint32_t t = 2 * kMove + 10; t <= 60000; t += 100) rig.tick(t);
  CHECK(rig.app().state().indication == SI::Clear);
  CHECK(rec.timeline == "CLEAR ");  // no STOP, no fault: no timer in the core
}

PARITY_SCENARIO(T, "a head reported failed at restart raises HW_ERROR before CONFIG and holds the state") {
  Rec rec;
  rig.driver().fail(0, true);
  rig.app().setObserver(&rec);
  for (uint32_t t = 0; t <= 2000; t += 50) rig.tick(t);
  CHECK(rec.timeline == "Fault Config ");
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::HwError);
}
PARITY_SCENARIO(T, "ADVANCE_APPROACH flashes at mast.flash_hz without flickering the state") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().setIndication(SI::AdvanceApproach, true);
  rig.tick(2 * kMove);  // t = 600: on-phase since 300
  CHECK(rig.app().state().indication == SI::AdvanceApproach);
  // 1 Hz: the phase flips every 500 ms counted from the command at t = 300.
  rig.tick(kMove + 499);
  CHECK(shows(rig, ColorName::Yellow));
  rig.tick(kMove + 500);
  CHECK(shows(rig, ColorName::None));
  rig.tick(kMove + 1000);
  CHECK(shows(rig, ColorName::Yellow));
  CHECK(rec.timeline == "ADVANCE_APPROACH ");
  // flash_hz is RW: accepted ONLINE.
  rig.app().setAttribute("mast.flash_hz", SetOp::Assign, AttrValue::number(200));  // 2.00 Hz
  rig.tick(kMove + 1249);
  CHECK(shows(rig, ColorName::Yellow));
  rig.tick(kMove + 1250);
  CHECK(shows(rig, ColorName::None));
}

PARITY_SCENARIO(T, "approach lighting: dark keeps the indication, and a dark mast can be re-aspected") {
  Rec rec;
  boot(rig, rec, 2);
  rig.app().setIndication(SI::Approach, true);
  rig.tick(2 * kMove);
  rig.app().setLit(false);
  rig.tick(2 * kMove + 1);
  CHECK(rig.app().state() == (MastState{SI::Approach, false}));
  CHECK(!rig.driver().lit());
  CHECK(shows(rig, ColorName::None, ColorName::None));
  rig.app().setIndication(SI::Clear, false);  // update while dark
  CHECK(!rig.driver().lit());
  CHECK(rig.driver().commanded(0) == ColorName::Green);
  rig.tick(2 * kMove + 2);  // head.A turning while dark: APPROACH (LIT) held
  CHECK(rig.app().state() == (MastState{SI::Approach, false}));
  rig.tick(4 * kMove);
  CHECK(rig.app().state() == (MastState{SI::Clear, false}));
  rig.app().setLit(true);
  rig.tick(4 * kMove + 1);
  CHECK(rig.app().state() == (MastState{SI::Clear, true}));
  CHECK(shows(rig, ColorName::Green, ColorName::Red));
  CHECK(rec.timeline == "APPROACH APPROACH (LIT) CLEAR (LIT) CLEAR ");
}
PARITY_SCENARIO(T, "three-head mast (Christopher 814EABC): STOP, CLEAR, and diverging aspects hold head.C RED") {
  Rec rec;
  boot(rig, rec, 3);
  CHECK(shows(rig, ColorName::Red, ColorName::Red, ColorName::Red));
  rig.app().setIndication(SI::Clear, true);
  CHECK(shows(rig, ColorName::Green, ColorName::Red, ColorName::Red));
  rig.tick(2 * kMove);
  CHECK(rig.app().state() == (MastState{SI::Clear, true}));
  rig.app().setIndication(SI::DivergingApproach, true);
  CHECK(shows(rig, ColorName::Red, ColorName::Yellow, ColorName::Red));
  rig.tick(4 * kMove);
  CHECK(rig.app().state() == (MastState{SI::DivergingApproach, true}));
}

PARITY_SCENARIO(T, "mast.heads and mast.rulebook are VITAL; heads stay within what the rulebook covers (1..3)") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().setAttribute("mast.heads", SetOp::Assign, AttrValue::number(2));  // ONLINE: ignored
  CHECK(rec.events.empty());
  rig.app().offline();
  rig.app().setAttribute("mast.heads", SetOp::Assign, AttrValue::number(0));
  rig.app().setAttribute("mast.heads", SetOp::Assign, AttrValue::number(4));  // no SP-1990 rows for four heads
  rig.app().setAttribute("mast.heads", SetOp::Increment, AttrValue::number(3));
  CHECK(rec.count(EventKind::Config) == 0);
  rig.app().setAttribute("mast.heads", SetOp::Assign, AttrValue::number(3));
  CHECK(rec.lastValue(EventKind::Config, "mast.heads") == 3);
  CHECK(shows(rig, ColorName::Red, ColorName::Red, ColorName::Red));  // all three heads show STOP at once
  CHECK(rec.lastValue(EventKind::Config, "mast.rulebook") == static_cast<uint32_t>(Rulebook::Sp1990));
  CHECK(rec.lastValue(EventKind::Config, "mast.flash_hz") == 100);
}

PARITY_SCENARIO(T, "four heads in NV is out of range: CONFIG_ERROR, one head at STOP, cleared once repaired") {
  Rec rec;
  rig.nv().save("mast.heads", 4);
  rig.app().setObserver(&rec);
  rig.tick(0);
  rig.tick(kMove);
  CHECK(rec.timeline == "Fault Config STOP ");
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::ConfigError);
  CHECK(rec.lastValue(EventKind::Config, "mast.heads") == 1);
  CHECK(rig.driver().commanded(0) == ColorName::Red);
  CHECK(rig.driver().commanded(1) == ColorName::None);
  rig.app().offline();
  rig.app().clearFault();  // NV still says four: stays faulted
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 1);
  rig.app().setAttribute("mast.heads", SetOp::Assign, AttrValue::number(3));
  rig.app().clearFault();
  rig.app().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 0);
}

PARITY_SCENARIO(T, "mast.flash_hz is bounded: 0.01 .. 10.00 Hz, written ONLINE") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().setAttribute("mast.flash_hz", SetOp::Assign, AttrValue::number(1001));  // 10.01 Hz
  rig.app().setAttribute("mast.flash_hz", SetOp::Assign, AttrValue::number(65535));
  rig.app().setAttribute("mast.flash_hz", SetOp::Assign, AttrValue::number(0));
  CHECK(rec.count(EventKind::Config) == 0);
  rig.app().setAttribute("mast.flash_hz", SetOp::Assign, AttrValue::number(1000));
  CHECK(rec.lastValue(EventKind::Config, "mast.flash_hz") == 1000);
  rig.app().setIndication(SI::AdvanceApproach, true);  // 10 Hz: 50 ms half period
  rig.tick(kMove + 49);
  CHECK(rig.driver().commanded(0) == ColorName::Yellow);
  rig.tick(kMove + 50);
  CHECK(rig.driver().commanded(0) == ColorName::None);
}

PARITY_SCENARIO(T, "a flashing head with a dead lamp: HW_ERROR, and its dark half never proves the proceed") {
  Rec rec;
  boot(rig, rec, 1);
  rig.driver().killLamp(0, true);  // a color-light head proven by lamp current
  rig.app().setIndication(SI::AdvanceApproach, true);
  bool proceed = false;
  for (uint32_t t = kMove + 10; t <= kMove + 5000; t += 10) {
    rig.tick(t);
    proceed |= rig.app().state().indication != SI::Stop;
  }
  CHECK(!proceed);
  CHECK(rec.timeline == "Fault ");  // once, though the lamp is read failed every lit half
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::HwError);
  rig.driver().killLamp(0, false);  // relamped: proven in the next lit half
  for (uint32_t t = kMove + 5010; t <= kMove + 6100; t += 10) rig.tick(t);
  CHECK(rig.app().state().indication == SI::AdvanceApproach);
}
PARITY_SCENARIO(T, "a flashing aspect is unproven until its first lit half proves it") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().setIndication(SI::Approach, true);
  rig.tick(2 * kMove);
  CHECK(rig.app().state().indication == SI::Approach);
  rig.driver().stick(0, true);  // will not prove from here on, and says nothing
  rig.app().setIndication(SI::AdvanceApproach, true);  // same color: no move. At t = 600: lit half 600..1099
  rig.tick(2 * kMove + 500);  // the first tick after it already lands in the dark half
  CHECK(rig.app().state().indication == SI::Approach);  // held, not taken from the dark half
}
PARITY_SCENARIO(T, "a dark mast holds a flashing head steady") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().setIndication(SI::AdvanceApproach, false);
  const uint32_t sets = rig.driver().setCount(0);
  bool steady = true;
  for (uint32_t t = kMove + 10; t <= kMove + 3000; t += 10) {
    rig.tick(t);
    steady &= rig.driver().commanded(0) == ColorName::Yellow;
  }
  CHECK(steady);
  CHECK(rig.driver().setCount(0) == sets);
  CHECK(rig.app().state() == (MastState{SI::AdvanceApproach, false}));
}

PARITY_SCENARIO(T, "cycle steps through every rulebook indication OFFLINE, then returns to STOP") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().cycle(1, 500);  // ONLINE: ignored
  rig.tick(kMove + 10);
  CHECK(rig.driver().setCount(0) == 1);
  rig.app().offline();
  rig.app().cycle(1, 500);
  rec.clear();
  for (uint32_t t = 400; t <= 10000; t += 50) rig.tick(t);
  CHECK(rec.timeline == "APPROACH ADVANCE_APPROACH CLEAR STOP ");  // no STOP while the head turns
  CHECK(shows(rig, ColorName::Red));
}

PARITY_SCENARIO(T, "a command before power-on is dropped and never reaches the heads") {
  Rec rec;
  rig.app().setObserver(&rec);
  rig.app().setIndication(SI::Clear, true);
  rig.app().setLit(false);
  rig.app().identify(IdentifyMode::On, 0);
  CHECK(rig.driver().setCount(0) == 0);
  rig.tick(0);
  rig.tick(5 * kMove);
  CHECK(shows(rig, ColorName::Red));
  CHECK(rig.app().state() == (MastState{SI::Stop, true}));
  CHECK(!rig.driver().beacon());
}

PARITY_SCENARIO(T, "online() while CLEAR and dark: CONFIG, ONLINE, CLEAR (LIT) held, then STOP once proven") {
  Rec rec;
  boot(rig, rec, 2);
  rig.app().offline();
  rig.app().setIndication(SI::Clear, false);
  rig.tick(3 * kMove);
  rec.clear();
  rig.app().online();
  CHECK(rec.timeline == "Config Online ");
  CHECK(rig.app().state() == (MastState{SI::Clear, false}));  // the prior confirmed state
  CHECK(rig.driver().lit());
  CHECK(shows(rig, ColorName::Red, ColorName::Red));
  rig.tick(4 * kMove - 1);
  CHECK(rec.timeline == "Config Online ");
  rig.tick(4 * kMove);
  CHECK(rec.timeline == "Config Online STOP ");
}

PARITY_SCENARIO(T, "online() with the heads already red: STOP proven at once, in the restart order") {
  Rec rec;
  boot(rig, rec, 1);
  rig.app().offline();
  rec.clear();
  rig.app().online();
  CHECK(rec.timeline == "STOP Config Online ");  // reported although unchanged
  rig.tick(kMove + 10);
  CHECK(rec.timeline == "STOP Config Online ");
}
PARITY_SCENARIO(T, "flash cadence holds across the uint32 clock wrap and after a long gap") {
  Rec rec;
  boot(rig, rec, 1);
  rig.tick(0xFFFFFC00u);
  rig.app().setIndication(SI::AdvanceApproach, true);  // phase origin 0xFFFFFC00
  rig.tick(0xFFFFFC00u + 499);
  CHECK(shows(rig, ColorName::Yellow));
  rig.tick(0xFFFFFC00u + 500);
  CHECK(shows(rig, ColorName::None));
  rig.tick(0xFFFFFC00u + 1000);  // wrapped past zero
  CHECK(shows(rig, ColorName::Yellow));
  const uint32_t sets = rig.driver().setCount(0);
  rig.tick(0x40000000u);  // a long gap: one flip, no catch-up burst
  CHECK(rig.driver().setCount(0) == sets + 1);
  CHECK(rig.app().state().indication == SI::AdvanceApproach);
}

PARITY_SCENARIO(T, "identify pulses the beacon and never changes the heads") {
  Rec rec;
  boot(rig, rec, 1);
  const uint32_t sets = rig.driver().setCount(0);
  rig.app().identify(IdentifyMode::Pulse, 300);
  CHECK(rig.driver().beacon());
  rig.tick(kMove + 300);
  CHECK(!rig.driver().beacon());
  CHECK(rig.driver().setCount(0) == sets);
}

PARITY_SCENARIO(T, "health: unproven, OK once attributes prove it, faulted, re-proven after ONLINE") {
  HealthBinding<T> b;
  rig.nv().save("mast.heads", 9);
  rig.app().setObserver(&b);
  scan(rig, b, 0);
  CHECK(b.health.health() == Health::Unproven);  // no state yet: STOP not proven
  scan(rig, b, kMove);
  CHECK(b.health.health() == Health::Faulted);
  rig.app().offline();
  rig.app().setAttribute("mast.heads", SetOp::Assign, AttrValue::number(2));
  rig.app().online();
  CHECK(b.health.health() == Health::Unproven);
  scan(rig, b, kMove + 10);
  CHECK(b.health.ok());
}

PARITY_SCENARIO(T, "health after a fault: OFFLINE, repair, CLEAR FAULT, ONLINE ends Ok, never Ok before ONLINE") {
  rig.nv().save("mast.heads", 9);
  maintainerClearsFault(rig, [](IMast& m) { m.setAttribute("mast.heads", SetOp::Assign, AttrValue::number(2)); });
}

int main() { return codeline::scenario::run(); }
