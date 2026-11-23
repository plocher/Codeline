// The five vital engine defects the M1c spike found in the donor engine
// (docs/research/engine-closed-loop-spike.md section 5). Each scenario
// shows the defect's situation and checks the corrected behavior.
#include "codeline/testing/EngineRig.h"
#include "scenario.h"

using namespace codeline;
using namespace codeline::testing;

// ---- Defect 1: opposing signals must lock each other out (RGKR) -----------

SCENARIO("defect 1 two opposing signals coded in one frame: neither clears") {
  SpikeRig r(1000, /*withWest=*/true);
  // The donor engine cleared both (probe_opposing). The frame is unsafe as
  // a whole (review finding 9): declaration order picks no winner.
  CHECK(r.code(Word::Normal, Word::Right, Word::Left) == ControlResult::VitalIgnored);
  r.run(500);
  CHECK(r.m2R.state().indication == SignalIndication::Stop);
  CHECK(r.m4L.state().indication == SignalIndication::Stop);
  CHECK(r.sig2.active() == Direction::None);
  CHECK(r.sig4.active() == Direction::None);
}

SCENARIO("defect 1 an opposing signal cannot be coded while the other is clear") {
  SpikeRig r(1000, true);
  CHECK(r.code(Word::Normal, Word::Right) == ControlResult::Applied);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
  CHECK(r.code(kNone, kNone, Word::Left) == ControlResult::VitalIgnored);
  r.run(300);
  CHECK(r.m4L.state().indication == SignalIndication::Stop);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
}

SCENARIO("defect 1 an opposing mast not proven at STOP blocks the other signal") {
  SpikeRig r(1000, true);
  r.m2R.setHealth(StandInHealth::Unavailable);  // could be showing anything
  r.step();
  CHECK(r.code(Word::Normal, kNone, Word::Left) == ControlResult::VitalIgnored);
  r.run(500);
  CHECK(r.m4L.state().indication == SignalIndication::Stop);
  r.m2R.setHealth(StandInHealth::Ok);
  r.run(300);
  CHECK(r.mast2.provenStop());
  CHECK(r.code(Word::Normal, kNone, Word::Left) == ControlResult::Applied);
  r.run(300);
  CHECK(r.m4L.state().indication == SignalIndication::Clear);
}

SCENARIO("defect 1 the opposing signal waits for the other route's time locking") {
  SpikeRig r(1000, true);
  r.s1A.shunt(true);
  r.code(Word::Normal, Word::Right);
  r.run(300);
  r.code(kNone, Word::Stop);
  r.run(1000);
  CHECK(r.cp.timeLocking(r.sig2));
  CHECK(r.code(kNone, kNone, Word::Left) == ControlResult::VitalIgnored);
  r.run(30000);
  CHECK(r.code(kNone, kNone, Word::Left) == ControlResult::Applied);
  r.run(300);
  CHECK(r.m4L.state().indication == SignalIndication::Clear);
}

// ---- Defect 2: approach locking whenever a signal drops in front of a train

SCENARIO("defect 2 a signal dropped by an occupied block ahead time-locks with a train approaching") {
  SpikeRig r;
  r.s1A.shunt(true);
  r.code(Word::Normal, Word::Right);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
  r.s1M.shunt(true);  // something fouls the block ahead; not a dispatcher cancel
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Stop);
  CHECK(!r.movable());  // the donor freed the switch in the same scan
  CHECK(r.code(Word::Reverse, kNone) == ControlResult::VitalIgnored);
  r.run(29000);
  CHECK(!r.movable());
  r.run(1500);
  CHECK(r.movable());
}

SCENARIO("defect 2 with the approach vacant the switch is freed once STOP is proven") {
  SpikeRig r;
  r.m2R.setConfirmMs(2000);
  r.code(Word::Normal, Word::Right);
  r.run(3000);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
  r.s1M.shunt(true);
  r.run(1000);
  CHECK(!r.movable());  // still displaying CLEAR: not proven at STOP
  r.run(1500);
  CHECK(r.m2R.state().indication == SignalIndication::Stop);
  CHECK(r.movable());  // proven, approach vacant: no time locking
}

// ---- Defect 3: the approach circuit in rear is for locking, not aspects ----

SCENARIO("defect 3 a train on the approach circuit does not downgrade the aspect") {
  SpikeRig r;
  r.s1A.shunt(true);
  r.code(Word::Normal, Word::Right);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);  // donor: APPROACH
}

SCENARIO("defect 3 the next signal at STOP caps the aspect at APPROACH") {
  SpikeRig r;
  r.nextMain.set(false);
  r.code(Word::Normal, Word::Right);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Approach);
  r.nextMain.set(true);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
  r.nextSide.set(false);
  r.code(kNone, Word::Stop);
  r.run(500);
  r.code(Word::Reverse, Word::Right);
  r.run(1500);
  CHECK(r.m2R.state().indication == SignalIndication::DivergingApproach);
}

// ---- Defect 4: indications pair with masts through the routes -------------

SCENARIO("defect 4 a lever's indication comes from its own route's mast, not the mast at its index") {
  StandInSensor sa, sb;
  StandInMast mb, ma;
  engine::ConstantFact go(true);
  engine::Plant cp("CP_PAIR");
  engine::TrackCircuit& ta = cp.addTrackCircuit("AT", sa);
  engine::TrackCircuit& tb = cp.addTrackCircuit("BT", sb);
  engine::SignalLever& la = cp.addSignalLever("A");  // lever index 0
  engine::SignalLever& lb = cp.addSignalLever("B");  // lever index 1
  engine::SignalMast& mastB = cp.addMast("BL", mb);  // mast index 0 belongs to B
  engine::SignalMast& mastA = cp.addMast("AL", ma);  // mast index 1 belongs to A
  cp.addRoute("A").shows(mastA, SignalIndication::Clear).governedBy(la, Direction::Left).clears({&ta}).approachLockedBy(ta).nextSignal(go);
  cp.addRoute("B").shows(mastB, SignalIndication::Clear).governedBy(lb, Direction::Left).clears({&tb}).approachLockedBy(tb).nextSignal(go);
  auto tick = [&](uint32_t now) {
    sa.tick(now);
    sb.tick(now);
    mb.tick(now);
    ma.tick(now);
    cp.tick(now);
  };
  tick(1000);
  tick(1100);
  engine::ControlTransaction c = cp.newControls();
  c.assertFunction(FunctionClass::Signal, "B", Word::Left);
  CHECK(cp.apply(c) == ControlResult::Applied);
  for (uint32_t t = 1200; t < 2000; t += 100) tick(t);
  CHECK(mb.state().indication == SignalIndication::Clear);
  CHECK(ma.state().indication == SignalIndication::Stop);
  engine::Indications ind = cp.indications();
  CHECK(ind.is(FunctionClass::Signal, "B", Word::Left));
  CHECK(!ind.is(FunctionClass::Signal, "A", Word::Left));
}

// ---- Defect 5: rollover-safe timers ------------------------------------------

SCENARIO("defect 5 time locking runs exactly 30 s across uint32_t rollover") {
  SpikeRig r(0xFFFFFFFFu - 10000u);
  r.s1A.shunt(true);
  r.code(Word::Normal, Word::Right);
  r.run(300);
  r.code(kNone, Word::Stop);
  r.step();
  r.step();  // mast commanded STOP, then proven: the TER starts here
  const uint32_t proof = r.t;
  CHECK(r.cp.timeLocking(r.sig2));
  r.run(29900);  // crosses 2^32 on the way
  CHECK(proof + 29900u < proof);  // the clock wrapped
  CHECK(!r.movable());
  r.run(200);
  CHECK(r.movable());
}

SCENARIO("defect 5 the travel timeout survives rollover") {
  SpikeRig r(0xFFFFFFFFu - 1000u);
  r.t1.setJammed(true);
  r.code(Word::Reverse, Word::Right);
  r.run(1500);  // wraps inside the 2 s travel window
  CHECK(r.sig2.active() == Direction::Right);  // still waiting for the points
  r.run(1000);
  CHECK(r.sig2.active() == Direction::None);   // dropped after 2 s, not at once, not never
}

int main() { return codeline::scenario::run(); }
