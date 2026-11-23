// The engine's office side against the Code Line Interface contract
// (docs/codeline/README.CodeLine.md): the stored-demand lifecycle of 5.1,
// the frame rules of 5.2 / 5.3, and detector, route, sectional and time
// locking as the office sees them through indications.
#include "codeline/testing/EngineRig.h"
#include "scenario.h"

using namespace codeline;
using namespace codeline::testing;

// ---- 5.1 rule 1: refused or unfulfilled demands are dropped ----------------

SCENARIO("5.1 rule 1 a demand refused after its switch settles is dropped, not stacked") {
  SpikeRig r;
  r.t1.setTravelMs(1500);
  CHECK(r.code(Word::Reverse, Word::Right) == ControlResult::Applied);
  r.run(500);
  r.s1S.shunt(true);  // the siding fouls while the points move
  r.run(2500);
  CHECK(r.sw.provenAt(Position::Reverse));
  CHECK(r.sig2.active() == Direction::None);
  r.s1S.shunt(false);  // hazard gone
  r.run(2000);
  CHECK(r.m2R.state().indication == SignalIndication::Stop);
  CHECK(r.code(kNone, Word::Right) == ControlResult::Applied);  // the dispatcher codes again
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::DivergingClear);
}

SCENARIO("5.1 rule 1 a signal demand with no route lined is dropped") {
  SpikeRig r;  // lever 4 has no routes in this rig
  CHECK(r.code(kNone, kNone, Word::Left) == ControlResult::Applied);
  r.step();
  CHECK(r.sig4.active() == Direction::None);
}

// ---- 5.1 rule 2: stick drop on train passage --------------------------------

SCENARIO("5.1 rule 2 a train accepting the signal drops it; no re-clear without a new code") {
  SpikeRig r;
  r.s1A.shunt(true);
  r.code(Word::Normal, Word::Right);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
  r.s1T.shunt(true);  // the train enters the OS
  r.s1A.shunt(false);
  r.step();
  CHECK(r.mast2.demanded() == SignalIndication::Stop);
  CHECK(r.sig2.active() == Direction::None);
  CHECK(r.cp.findRoute("MAIN")->state() == Route::State::Traversing);
  r.s1M.shunt(true);
  r.step();
  r.s1T.shunt(false);
  r.run(300);
  CHECK(r.movable());  // section released behind the train: sectional release
  CHECK(!r.cp.timeLocking(r.sig2));  // no time locking for an accepted signal
  r.s1M.shunt(false);
  r.run(1000);
  CHECK(r.m2R.state().indication == SignalIndication::Stop);  // no automatic re-clear
  CHECK(r.code(kNone, Word::Right) == ControlResult::Applied);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
}

SCENARIO("5.1 rule 2 a switch stays locked while a slow mast still shows proceed behind the train") {
  SpikeRig r;
  r.m2R.setConfirmMs(3000);
  r.code(Word::Normal, Word::Right);
  r.run(3500);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
  r.s1T.shunt(true);
  r.step();
  r.s1T.shunt(false);  // a short, fast train clears the whole route at once
  r.step();
  CHECK(r.cp.findRoute("MAIN")->state() == Route::State::Idle);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);  // still lit
  CHECK(!r.movable());  // change 7: a mast not proven at STOP holds the switch
  r.run(3500);
  CHECK(r.m2R.state().indication == SignalIndication::Stop);
  CHECK(r.movable());
}

// ---- 5.1 rule 3: field restart -------------------------------------------------

SCENARIO("5.1 rule 3 a field restart clears every demand and time-locks a lined route with a train approaching") {
  SpikeRig r;
  r.s1A.shunt(true);
  r.code(Word::Normal, Word::Right);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);

  // The field logic restarts; the appliances do not. A new plant binds the
  // same appliances, which replay their retained state on reconnect.
  engine::ConstantFact go(true);
  engine::Plant cp("CP_M1C");
  engine::TrackCircuit& os = cp.addTrackCircuit("1T", r.s1T);
  engine::TrackCircuit& appr = cp.addTrackCircuit("1A", r.s1A);
  engine::TrackCircuit& main = cp.addTrackCircuit("1M", r.s1M);
  engine::Switch& sw = cp.addPowerSwitch("1", r.t1, &os);
  engine::SignalLever& sig2 = cp.addSignalLever("2");
  engine::SignalMast& mast2 = cp.addMast("2R", r.m2R);
  cp.addRoute("MAIN").shows(mast2, SignalIndication::Clear).governedBy(sig2, Direction::Right)
      .aligns(sw, Position::Normal, &os).clears({&os, &main}).approachLockedBy(appr).nextSignal(go);
  r.m2R.setConfirmMs(1000);
  for (StandInHealth h : {StandInHealth::Unavailable, StandInHealth::Ok}) {
    r.s1T.setHealth(h);
    r.s1A.setHealth(h);
    r.s1M.setHealth(h);
    r.t1.setHealth(h);
    r.m2R.setHealth(h);
  }
  auto tick = [&](uint32_t now) {
    r.s1T.tick(now);
    r.s1A.tick(now);
    r.s1M.tick(now);
    r.t1.tick(now);
    r.m2R.tick(now);
    cp.tick(now);
  };
  uint32_t t = r.t;
  tick(t += 100);  // state replayed; health asked for
  tick(t += 100);  // health proven
  engine::Indications ind = cp.indications();
  CHECK(sig2.active() == Direction::None);       // every stored demand cleared
  CHECK(mast2.demanded() == SignalIndication::Stop);
  CHECK(sw.provenAt(Position::Normal));          // switches stay where they are
  CHECK(!sw.WLR());                              // the mast is not yet proven at STOP
  CHECK(ind.is(FunctionClass::Signal, "2", Word::Right));  // honest: it still shows CLEAR
  uint32_t proof = 0;
  while (t < r.t + 5000) {
    tick(t += 100);
    if (!proof && mast2.provenStop()) proof = t;
  }
  CHECK(proof != 0);
  CHECK(!sw.WLR());
  ind = cp.indications();
  CHECK(ind.is(FunctionClass::Signal, "2", Word::Time));
  CHECK(!ind.is(FunctionClass::Signal, "2", Word::Right));
  while (t < proof + 14900) tick(t += 100);  // the default TER, 15 s
  CHECK(!sw.WLR());
  tick(t += 200);
  CHECK(sw.WLR());
  CHECK(!cp.indications().is(FunctionClass::Signal, "2", Word::Time));
}

// ---- 5.2 / 5.3: what the field does with a frame -------------------------------

SCENARIO("5.3 an unsafe frame ignores every vital control, even a STOP, but applies the maintainer call") {
  SpikeRig r;
  r.code(Word::Normal, Word::Right);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
  engine::ControlTransaction c = r.cp.newControls();
  c.assertFunction(FunctionClass::Switch, "1", Word::Reverse);  // route locked: unsafe
  c.assertFunction(FunctionClass::Signal, "2", Word::Stop);     // safe on its own
  c.assertFunction(FunctionClass::MaintainerCall, "1MC", Word::Call);
  CHECK(r.cp.apply(c) == ControlResult::VitalIgnored);
  r.run(500);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
  CHECK(r.sw.demand() == Position::Normal);
  CHECK(r.t1.lastCommand() != TurnoutCommand::Reverse);
  CHECK(r.cp.maintainerCall("1MC"));
  CHECK(r.cp.indications().is(FunctionClass::MaintainerCall, "1MC", Word::Call));
}

SCENARIO("5.3 a signal coded over an occupied OS is unsafe and moves nothing") {
  SpikeRig r;
  r.s1T.shunt(true);
  r.step();
  CHECK(r.code(Word::Normal, Word::Right) == ControlResult::VitalIgnored);
  r.run(300);
  CHECK(r.sig2.active() == Direction::None);
  CHECK(r.t1.commands() == 0);
}

SCENARIO("5.2 two words of one group make the frame malformed: nothing applied, not even the call") {
  SpikeRig r;
  engine::ControlTransaction c = r.cp.newControls();
  c.assertFunction(FunctionClass::Switch, "1", Word::Normal);
  c.assertFunction(FunctionClass::Switch, "1", Word::Reverse);
  c.assertFunction(FunctionClass::MaintainerCall, "1MC", Word::Call);
  CHECK(c.malformed());
  CHECK(r.cp.apply(c) == ControlResult::Malformed);
  CHECK(!r.cp.maintainerCall("1MC"));
  engine::ControlTransaction d = r.cp.newControls();
  d.assertFunction(FunctionClass::Signal, "2", Word::Left);
  d.assertFunction(FunctionClass::Signal, "2", Word::Stop);
  CHECK(r.cp.apply(d) == ControlResult::Malformed);
  engine::ControlTransaction e = r.cp.newControls();
  e.assertFunction(FunctionClass::Signal, "99", Word::Left);  // no such function
  CHECK(r.cp.apply(e) == ControlResult::Malformed);
}

SCENARIO("5.2 a group with nothing asserted leaves the demand unchanged; a call is a level") {
  SpikeRig r;
  r.code(Word::Reverse, kNone);
  r.run(1000);
  CHECK(r.sw.provenAt(Position::Reverse));
  engine::ControlTransaction c = r.cp.newControls();
  c.assertFunction(FunctionClass::MaintainerCall, "1MC", Word::Call);
  CHECK(r.cp.apply(c) == ControlResult::Applied);
  r.run(1000);
  CHECK(r.sw.provenAt(Position::Reverse));
  CHECK(r.cp.maintainerCall("1MC"));
  CHECK(r.code(kNone, kNone) == ControlResult::Applied);  // CALL dropped
  CHECK(!r.cp.maintainerCall("1MC"));
}

// ---- locking, as the office sees it -------------------------------------------

SCENARIO("locking: switch lamps go dark in transit; route locking refuses a throw under a clear signal") {
  SpikeRig r;
  r.t1.setTravelMs(1000);
  r.code(Word::Reverse, kNone);
  r.run(300);
  engine::Indications ind = r.cp.indications();
  CHECK(!ind.is(FunctionClass::Switch, "1", Word::Normal));
  CHECK(!ind.is(FunctionClass::Switch, "1", Word::Reverse));
  r.run(1000);
  CHECK(r.cp.indications().is(FunctionClass::Switch, "1", Word::Reverse));
  r.code(kNone, Word::Right);
  r.run(300);
  CHECK(r.cp.indications().is(FunctionClass::Signal, "2", Word::Right));
  CHECK((r.sw.locks() & engine::lock::kRoute) != 0);
  CHECK(r.code(Word::Normal, kNone) == ControlResult::VitalIgnored);
}

SCENARIO("locking: a cancel with the approach vacant releases at once, without TIME") {
  SpikeRig r;
  r.code(Word::Normal, Word::Right);
  r.run(300);
  r.code(kNone, Word::Stop);
  r.run(400);  // the STOP reaches the mast after its latency and is confirmed
  CHECK(r.movable());
  CHECK(!r.cp.indications().is(FunctionClass::Signal, "2", Word::Time));
}

int main() { return codeline::scenario::run(); }
