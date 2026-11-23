// Findings of the M2 engine review, each shown by a scenario, and the
// locking the review found untested (each scenario kills a named mutant of
// the review's mutation run).
#include <cstdint>
#include <initializer_list>

#include "codeline/testing/CorporalPlant.h"
#include "codeline/testing/EngineRig.h"
#include "scenario.h"

using namespace codeline;
using namespace codeline::testing;
using engine::ControlResult;
using engine::FunctionClass;
using engine::Position;
using engine::Route;
using engine::Word;

namespace {

struct Fn {
  FunctionClass cls;
  const char* appliance;
  Word word;
};

ControlResult frame(engine::Plant& p, std::initializer_list<Fn> fns) {
  engine::ControlTransaction c = p.newControls();
  for (const Fn& f : fns) c.assertFunction(f.cls, f.appliance, f.word);
  return p.apply(c);
}

const Fn k829N{FunctionClass::Switch, "829", Word::Normal};
const Fn k829R{FunctionClass::Switch, "829", Word::Reverse};
const Fn k827Release{FunctionClass::Lock, "827", Word::Release};
const Fn k828L{FunctionClass::Signal, "828", Word::Left};
const Fn k828R{FunctionClass::Signal, "828", Word::Right};
const Fn k830L{FunctionClass::Signal, "830", Word::Left};
const Fn k830R{FunctionClass::Signal, "830", Word::Right};
const Fn k830Stop{FunctionClass::Signal, "830", Word::Stop};

void settle(CorporalPlant& p) {
  p.boot();
  p.run(31000);  // restart time locking (829T1 approach unknown on the eastbound routes) runs out
}

// 830W cleared: 829 thrown normal, 830 and 828 LEFT.
void clear830W(CorporalPlant& p) {
  CHECK(frame(p.cp, {k829N, k828L, k830L}) == ControlResult::Applied);
  p.run(3000);
  CHECK(p.m830WAB.state().indication == SignalIndication::Approach);
}

}  // namespace

// ---- Finding 1: only a healthy, proven occupancy is a train accepting the signal

SCENARIO("review 1 an OS sensor blip under a clear signal is not a train: approach locking holds") {
  SpikeRig s;
  s.s1A.shunt(true);  // a train approaches
  CHECK(s.code(Word::Normal, Word::Right) == ControlResult::Applied);
  s.run(500);
  CHECK(s.m2R.state().indication == SignalIndication::Clear);
  s.s1T.setHealth(StandInHealth::Unavailable);  // link blip, no train on 1T
  s.step();
  CHECK(s.cp.findRoute("MAIN")->state() != Route::State::Traversing);
  s.s1T.setHealth(StandInHealth::Ok);
  s.run(1000);
  CHECK(s.m2R.state().indication == SignalIndication::Stop);
  CHECK(!s.movable());
  CHECK(s.cp.timeLocking(s.sig2));
  CHECK(s.cp.indications().is(FunctionClass::Signal, "2", Word::Time));
  CHECK(s.code(Word::Reverse, kNone) == ControlResult::VitalIgnored);
  s.run(1000);
  CHECK(s.t1.commands() == 0);
  CHECK(s.sw.provenAt(Position::Normal));
}

// ---- Finding 2: a section releases only on proven occupancy then proven vacancy

SCENARIO("review 2 a sensor blip ahead of the train does not release that section") {
  CorporalPlant p;
  settle(p);
  CHECK(frame(p.cp, {k829N}) == ControlResult::Applied);
  p.run(3000);
  CHECK(frame(p.cp, {k828R, k830R}) == ControlResult::Applied);
  p.run(1000);
  Route* r = p.cp.findRoute("828E");
  CHECK(r->state() == Route::State::Cleared);
  p.s827T1.shunt(true);  // the train accepts 828EA
  p.run(1000);
  CHECK(r->state() == Route::State::Traversing);
  p.s829T1.setHealth(StandInHealth::Unavailable);  // 829T1 blips; the train is still in 827T1
  p.step();
  p.s829T1.setHealth(StandInHealth::Ok);
  p.run(1000);
  CHECK(!p.sw829.WLR());
  CHECK(frame(p.cp, {k829R}) == ControlResult::VitalIgnored);
  p.run(500);
  CHECK(p.t829.lastCommand() != TurnoutCommand::Reverse);
  // The train runs on: 829T1 occupied then vacant releases 829.
  p.s829T1.shunt(true);
  p.run(500);
  p.s827T1.shunt(false);
  p.run(500);
  p.s829T1.shunt(false);
  p.s832T.shunt(true);
  p.run(500);
  CHECK(p.sw829.WLR());
}

SCENARIO("review 2 a section whose circuit is unhealthy when the train accepts the signal stays locked") {
  CorporalPlant p;
  settle(p);
  CHECK(frame(p.cp, {k829N}) == ControlResult::Applied);
  p.run(3000);
  CHECK(frame(p.cp, {k828R, k830R}) == ControlResult::Applied);
  p.run(1000);
  Route* r = p.cp.findRoute("828E");
  CHECK(r->state() == Route::State::Cleared);
  p.s827T1.shunt(true);                            // the train accepts 828EA
  p.s829T1.setHealth(StandInHealth::Unavailable);  // in the same scan 829T1 drops off the link
  p.run(500);
  CHECK(r->state() == Route::State::Traversing);
  p.s829T1.setHealth(StandInHealth::Ok);  // back, vacant: the train has not reached it
  p.run(1000);
  CHECK(!p.sw829.WLR());
  CHECK(frame(p.cp, {k829R}) == ControlResult::VitalIgnored);
}

SCENARIO("review 2 a section seen occupied is not released while its circuit is unhealthy") {
  // Switch 1 released by the circuit beyond it (1M), not by its OS.
  StandInSensor s1T, s1M;
  StandInTurnout t1;
  StandInMast m2;
  engine::ConstantFact go{true};
  engine::Plant cp("P");
  engine::TrackCircuit& os = cp.addTrackCircuit("1T", s1T);
  engine::TrackCircuit& m = cp.addTrackCircuit("1M", s1M);
  engine::Switch& sw = cp.addPowerSwitch("1", t1, &os);
  engine::SignalLever& l2 = cp.addSignalLever("2");
  engine::SignalMast& mast = cp.addMast("2R", m2);
  cp.addRoute("MAIN").shows(mast, SignalIndication::Clear).governedBy(l2, Direction::Right)
      .aligns(sw, Position::Normal, &m).clears({&os, &m}).nextSignal(go);
  uint32_t t = 1000;
  auto run = [&](int scans) {
    for (int i = 0; i < scans; ++i, t += 100) {
      s1T.tick(t);
      s1M.tick(t);
      t1.tick(t);
      m2.tick(t);
      cp.tick(t);
    }
  };
  run(400);  // restart time locking (no approach circuit) runs out
  CHECK(frame(cp, {{FunctionClass::Signal, "2", Word::Right}}) == ControlResult::Applied);
  run(5);
  CHECK(m2.displayed() == SignalIndication::Clear);
  s1T.shunt(true);
  run(3);
  s1M.shunt(true);
  run(3);
  s1T.shunt(false);  // the train is wholly in 1M
  run(3);
  CHECK(!sw.WLR());
  s1M.setHealth(StandInHealth::Unavailable);
  run(3);
  CHECK(!sw.WLR());  // not proven vacant: the section holds
}

// ---- Finding 3: a switch is driven only while this scan's locks allow it

SCENARIO("review 3 a throw accepted between scans is not sent when this scan finds the OS occupied") {
  SpikeRig s;
  s.run(1000);
  CHECK(s.movable());
  s.s1T.shunt(true);  // a hand move runs onto the OS
  CHECK(s.code(Word::Reverse, kNone) == ControlResult::Applied);  // judged on the last scan's locks
  s.step();  // this scan sees the OS occupied
  CHECK((s.sw.locks() & engine::lock::kDetector) != 0);
  s.run(1500);
  CHECK(s.t1.commands() == 0);
  CHECK(s.sw.demand() == Position::Normal);  // the demand is dropped
  CHECK(s.sw.provenAt(Position::Normal));
  s.s1T.shunt(false);
  s.run(1500);
  CHECK(s.t1.commands() == 0);  // and never sent later
}

SCENARIO("review 3 a demand stranded by a link drop is never sent on re-admission") {
  SpikeRig s;
  s.run(1000);
  CHECK(s.code(Word::Reverse, kNone) == ControlResult::Applied);
  s.t1.setHealth(StandInHealth::Unavailable);  // the link drops before the next scan drives it
  s.step();
  s.s1T.shunt(true);  // a train occupies the OS while the switch appliance is away
  s.step();
  s.t1.handThrow(TurnoutState::InTransit);  // contacts show neither end
  s.t1.setHealth(StandInHealth::Ok);
  s.run(2000);
  CHECK(s.t1.commands() == 0);
  CHECK(!s.sw.inCorrespondence());
  s.t1.handThrow(TurnoutState::Normal);  // a crew finishes the throw by hand
  s.run(500);
  CHECK(s.sw.provenAt(Position::Normal));  // adopted where found
  CHECK(s.t1.commands() == 0);
  s.s1T.shunt(false);
  s.run(1000);
  CHECK(s.t1.commands() == 0);
}

SCENARIO("review 3 a demand stranded by a link drop is not sent on re-admission, even with nothing locked") {
  SpikeRig s;
  s.run(1000);
  CHECK(s.code(Word::Reverse, kNone) == ControlResult::Applied);
  s.t1.setHealth(StandInHealth::Unavailable);  // the link drops before the next scan drives it
  s.step();
  s.t1.handThrow(TurnoutState::InTransit);
  s.t1.setHealth(StandInHealth::Ok);
  s.run(2000);
  CHECK(s.movable());
  CHECK(s.t1.commands() == 0);
  CHECK(s.sw.demand() == Position::Unknown);
}

// ---- Finding 4: the lock is proven locked only after the withdrawal took effect

SCENARIO("review 4 a frame that re-locks 827 and clears over it is unsafe") {
  CorporalPlant p;
  settle(p);
  CHECK(frame(p.cp, {k829N, k827Release}) == ControlResult::Applied);
  p.run(3000);
  CHECK(p.lock827.releaseLit());
  CHECK(frame(p.cp, {k829N, k828L, k830L}) == ControlResult::VitalIgnored);  // (827WLS) + 830W
  p.run(500);
  CHECK(p.lock827.releaseLit());  // nothing of it applied
  CHECK(p.m830WAB.displayed() == SignalIndication::Stop);
}

SCENARIO("review 4 a frame that releases 827 and clears over it is unsafe") {
  CorporalPlant p;
  settle(p);
  CHECK(frame(p.cp, {k829N, k827Release, k828L, k830L}) == ControlResult::VitalIgnored);
  p.run(3000);
  CHECK(!p.lock827.releaseLit());
  CHECK(p.m830WAB.displayed() == SignalIndication::Stop);
}

SCENARIO("review 4 830WAB never shows proceed while the 827 release lamp is lit, with a lamp 800 ms slow to go out") {
  CorporalPlant p;
  p.lock827.setLatencyMs(800);  // a slow remote CONTROL
  settle(p);
  CHECK(frame(p.cp, {k829N, k827Release}) == ControlResult::Applied);
  p.run(3000);
  CHECK(p.lock827.releaseLit());
  CHECK(frame(p.cp, {k829N}) == ControlResult::Applied);  // re-lock
  bool violated = false;
  bool cleared = false;
  for (int i = 0; i < 60; ++i) {  // the dispatcher codes 830W until it is accepted
    if (!cleared && frame(p.cp, {k829N, k828L, k830L}) == ControlResult::Applied) cleared = true;
    p.step();
    if (p.lock827.releaseLit() && p.m830WAB.displayed() != SignalIndication::Stop) violated = true;
  }
  CHECK(!violated);
  CHECK(cleared);
  CHECK(!p.lock827.releaseLit());
  CHECK(p.m830WAB.displayed() == SignalIndication::Approach);
}

SCENARIO("review 4 the lock is not proven locked in the scan that withdraws the release") {
  CorporalPlant p;
  settle(p);
  CHECK(frame(p.cp, {k829N, k827Release}) == ControlResult::Applied);
  p.run(3000);
  CHECK(frame(p.cp, {k829N}) == ControlResult::Applied);
  p.step();  // the withdrawal is sent in this scan
  CHECK(!p.el827.provenLocked());
  CHECK(p.cp.indications().is(FunctionClass::Lock, "827", Word::Released));
  CHECK(frame(p.cp, {k829N, k828L, k830L}) == ControlResult::VitalIgnored);
}

// ---- Finding 5: configuration after the first tick is refused

SCENARIO("review 5 a route added after the first tick is refused, detectably") {
  SpikeRig s;  // ticked: configuration frozen
  CHECK(!s.cp.configurationRefused());
  s.cp.addRoute("WEST")
      .shows(s.mast4, SignalIndication::Clear)
      .governedBy(s.sig4, Direction::Left)
      .aligns(s.sw, Position::Normal, &s.os)
      .clears({&s.os, &s.west})
      .approachLockedBy(s.main)
      .nextSignal(s.nextWest);
  CHECK(s.cp.configurationRefused());
  CHECK(s.cp.findRoute("WEST") == nullptr);
  s.run(1000);
  CHECK(s.code(Word::Normal, Word::Right) == ControlResult::Applied);
  s.run(500);
  s.code(kNone, kNone, Word::Left);
  s.run(500);
  CHECK(s.m2R.state().indication == SignalIndication::Clear);
  CHECK(s.m4L.state().indication == SignalIndication::Stop);
  s.s1T.shunt(true);  // the train enters: nothing indexes a late route
  s.run(500);
  CHECK(s.cp.findRoute("MAIN")->state() == Route::State::Traversing);
}

SCENARIO("review 5 every kind of late configuration is refused; a live route cannot be extended") {
  SpikeRig s;
  StandInSensor late;
  s.cp.addTrackCircuit("9T", late);
  CHECK(s.cp.configurationRefused());
  CHECK(s.cp.findTrackCircuit("9T") == nullptr);
  SpikeRig q;
  q.cp.findRoute("MAIN")->aligns(q.sw, Position::Reverse);
  CHECK(q.cp.configurationRefused());
  CHECK(q.cp.findRoute("MAIN")->switches().size() == 1);
  SpikeRig c;
  c.cp.addMaintainerCall("2MC");
  CHECK(c.cp.configurationRefused());
}

SCENARIO("review 5 a transaction built against another configuration is malformed") {
  StandInSensor os;
  StandInTurnout t;
  engine::Plant cp("CP");
  engine::TrackCircuit& tc = cp.addTrackCircuit("1T", os);
  cp.addPowerSwitch("1", t, &tc);
  cp.addMaintainerCall("1MC");
  engine::ControlTransaction c = cp.newControls();  // one call
  c.assertFunction(FunctionClass::MaintainerCall, "1MC", Word::Call);
  cp.addMaintainerCall("2MC");
  cp.addMaintainerCall("3MC");
  for (uint32_t now = 1000; now <= 1300; now += 100) {
    os.tick(now);
    t.tick(now);
    cp.tick(now);
  }
  CHECK(cp.apply(c) == ControlResult::Malformed);
  CHECK(!cp.maintainerCall("1MC"));
}

// ---- Finding 6: a demand is held only while switches of its own frame settle

SCENARIO("review 6 a refused demand is not held by a switch thrown in an earlier frame") {
  CorporalPlant p;
  settle(p);  // 829 found reversed
  CHECK(frame(p.cp, {k829N}) == ControlResult::Applied);  // frame A: 829 starts its 2 s throw
  p.step();
  p.m828EA.setHealth(StandInHealth::Unavailable);
  p.step();
  CHECK(frame(p.cp, {k828R, k830R}) == ControlResult::Applied);  // frame B: no switch
  p.step();
  CHECK(!p.sig828.pending());
  CHECK(p.sig828.active() == engine::Direction::None);
  p.run(1000);
  p.m828EA.setHealth(StandInHealth::Ok);
  p.run(3000);
  CHECK(p.cp.findRoute("828E")->state() == Route::State::Idle);
  CHECK(p.m828EA.displayed() == SignalIndication::Stop);
}

SCENARIO("review 6 a demand is held while a switch of its own frame settles, then clears") {
  CorporalPlant p;
  settle(p);  // 829 found reversed
  CHECK(frame(p.cp, {k829N, k828R, k830R}) == ControlResult::Applied);
  p.run(1000);
  CHECK(p.sig828.pending());
  p.run(2500);
  CHECK(p.cp.findRoute("828E")->state() == Route::State::Cleared);
}

// ---- Finding 7: GK follows any healthy mast showing proceed

SCENARIO("review 7 GK stays up while a healthy mast still shows proceed behind an idle route") {
  SpikeRig s;
  s.run(1000);
  s.code(Word::Normal, Word::Right);
  s.run(500);
  CHECK(s.cp.indications().is(FunctionClass::Signal, "2", Word::Right));
  s.m2R.setStuck(true);  // the heads fail to assume STOP
  s.s1T.shunt(true);
  s.step();
  s.s1M.shunt(true);
  s.step();
  s.s1T.shunt(false);
  s.step();
  s.s1M.shunt(false);
  s.run(1000);
  CHECK(s.cp.findRoute("MAIN")->state() == Route::State::Idle);
  CHECK(s.m2R.state().indication == SignalIndication::Clear);
  CHECK(s.cp.indications().is(FunctionClass::Signal, "2", Word::Right));
  CHECK(!s.movable());
}

// ---- Finding 9: two conflicting signals in one frame

SCENARIO("review 9 two opposing signals in one frame are unsafe, whatever the declaration order") {
  for (int order = 0; order < 2; ++order) {
    StandInSensor sT, sE, sW;
    StandInTurnout t1;
    StandInMast mE, mW;
    engine::ConstantFact go{true};
    engine::Plant cp("P");
    engine::TrackCircuit& T = cp.addTrackCircuit("1T", sT);
    engine::TrackCircuit& E = cp.addTrackCircuit("E", sE);
    engine::TrackCircuit& W = cp.addTrackCircuit("W", sW);
    engine::Switch& sw = cp.addPowerSwitch("1", t1, &T);
    engine::SignalLever& l2 = cp.addSignalLever("2");
    engine::SignalLever& l4 = cp.addSignalLever("4");
    engine::SignalMast& m2 = cp.addMast("2R", mE);
    engine::SignalMast& m4 = cp.addMast("4L", mW);
    auto east = [&] {
      cp.addRoute("EAST").shows(m2, SignalIndication::Clear).governedBy(l2, Direction::Right)
          .aligns(sw, Position::Normal, &T).clears({&T, &E}).approachLockedBy(W).nextSignal(go);
    };
    auto west = [&] {
      cp.addRoute("WEST").shows(m4, SignalIndication::Clear).governedBy(l4, Direction::Left)
          .aligns(sw, Position::Normal, &T).clears({&T, &W}).approachLockedBy(E).nextSignal(go);
    };
    if (order == 0) {
      east();
      west();
    } else {
      west();
      east();
    }
    uint32_t t = 1000;
    auto tick = [&] {
      sT.tick(t);
      sE.tick(t);
      sW.tick(t);
      t1.tick(t);
      mE.tick(t);
      mW.tick(t);
      cp.tick(t);
    };
    for (int i = 0; i < 20; ++i, t += 100) tick();
    CHECK(frame(cp, {{FunctionClass::Signal, "2", Word::Right}, {FunctionClass::Signal, "4", Word::Left}}) ==
          ControlResult::VitalIgnored);
    for (int i = 0; i < 10; ++i, t += 100) tick();
    CHECK(mE.displayed() == SignalIndication::Stop);
    CHECK(mW.displayed() == SignalIndication::Stop);
    CHECK(frame(cp, {{FunctionClass::Signal, "2", Word::Right}}) == ControlResult::Applied);
    for (int i = 0; i < 10; ++i, t += 100) tick();
    CHECK(mE.displayed() == SignalIndication::Clear);
  }
}

// ---- Finding 10: the travel timeout is latched

SCENARIO("review 10 a jammed switch never reads settling again, even after 49.7 days") {
  SpikeRig s;
  s.run(1000);
  s.t1.setJammed(true);
  s.code(Word::Reverse, kNone);
  s.run(6000);
  CHECK(!s.sw.settling());
  bool again = false;
  const uint64_t limit = (1ull << 32) + 20000;
  for (uint64_t since = 6000; since < limit;) {
    const uint32_t st = (limit - since > 300000) ? 60000 : 500;
    s.step(st);
    since += st;
    again = again || s.sw.settling();
  }
  CHECK(!again);
}

// ---- Finding 11: nothing acts on a frame before the first tick

SCENARIO("review 11 a frame before the plant's first tick is not acted on, not even the call") {
  CorporalPlant p;  // not ticked
  CHECK(frame(p.cp, {{FunctionClass::MaintainerCall, "1MC", Word::Call}}) != ControlResult::Applied);
  CHECK(!p.cp.maintainerCall("1MC"));
  p.boot();
  CHECK(!p.cp.maintainerCall("1MC"));
  CHECK(!p.cp.indications().is(FunctionClass::MaintainerCall, "1MC", Word::Call));
}

// ---- Test gaps -------------------------------------------------------------------

SCENARIO("gap M47 830W needs 828 LEFT as well as 830 LEFT") {
  CorporalPlant p;
  settle(p);
  CHECK(frame(p.cp, {k829N, k830L}) == ControlResult::Applied);
  p.run(3000);
  CHECK(p.m830WAB.displayed() == SignalIndication::Stop);
  CHECK(p.sig830.active() == engine::Direction::None);  // unfulfilled: dropped
}

SCENARIO("gap M28 lock 827 faulting after 830WAB cleared drops the signal for good") {
  CorporalPlant p;
  settle(p);
  clear830W(p);
  p.lock827.setHealth(StandInHealth::Faulted);
  p.run(1000);
  CHECK(p.mast830WAB.demanded() == SignalIndication::Stop);
  CHECK(p.m830WAB.displayed() == SignalIndication::Stop);
  p.lock827.setHealth(StandInHealth::Ok);
  p.run(3000);
  CHECK(p.m830WAB.displayed() == SignalIndication::Stop);  // no re-clear
}

SCENARIO("gap M18 lock contacts that do not read as one channel read open") {
  CorporalPlant p;
  settle(p);
  clear830W(p);
  p.lock827.setContactCount(2);  // wrong profile: two channels, SW1 closed
  p.run(1000);
  CHECK(!p.el827.provenLocked());
  CHECK(p.cp.indications().is(FunctionClass::Lock, "827", Word::Released));
  CHECK(p.m830WAB.displayed() == SignalIndication::Stop);
  p.run(31000);
  CHECK(frame(p.cp, {k829N, k828L, k830L}) == ControlResult::VitalIgnored);
}

SCENARIO("gap M09 a lock release is refused under time locking") {
  CorporalPlant p;
  settle(p);
  clear830W(p);
  p.s832T.shunt(true);  // a train approaches 830WAB
  p.step();
  CHECK(frame(p.cp, {k829N, k830Stop}) == ControlResult::Applied);
  p.run(1000);
  CHECK(p.cp.findRoute("830W")->state() == Route::State::Timing);
  CHECK(frame(p.cp, {k829N, k827Release}) == ControlResult::VitalIgnored);
  p.run(500);
  CHECK(!p.lock827.releaseLit());
}

SCENARIO("gap M09 a lock release is refused with 827T1 occupied") {
  CorporalPlant p;
  settle(p);
  p.s827T1.shunt(true);
  p.step();
  CHECK(frame(p.cp, {k827Release}) == ControlResult::VitalIgnored);
  p.run(500);
  CHECK(!p.lock827.releaseLit());
}

SCENARIO("gap M09 a lock release is refused while 830WAB is not proven at STOP") {
  CorporalPlant p;
  settle(p);
  p.m830WAB.setHealth(StandInHealth::Offline);
  p.step();
  CHECK(frame(p.cp, {k827Release}) == ControlResult::VitalIgnored);
  p.run(500);
  CHECK(!p.lock827.releaseLit());
}

SCENARIO("gap M45 two signals facing each other over one block, with no switch, conflict") {
  StandInSensor sB, sE, sW;
  StandInMast mE, mW;
  engine::ConstantFact go{true};
  engine::Plant cp("P");
  engine::TrackCircuit& B = cp.addTrackCircuit("B", sB);
  engine::TrackCircuit& E = cp.addTrackCircuit("E", sE);
  engine::TrackCircuit& W = cp.addTrackCircuit("W", sW);
  engine::SignalLever& l2 = cp.addSignalLever("2");
  engine::SignalLever& l4 = cp.addSignalLever("4");
  engine::SignalMast& m2 = cp.addMast("2R", mE);
  engine::SignalMast& m4 = cp.addMast("4L", mW);
  cp.addRoute("EAST").shows(m2, SignalIndication::Clear).governedBy(l2, Direction::Right).clears({&B})
      .approachLockedBy(W).nextSignal(go);
  cp.addRoute("WEST").shows(m4, SignalIndication::Clear).governedBy(l4, Direction::Left).clears({&B})
      .approachLockedBy(E).nextSignal(go);
  uint32_t t = 1000;
  auto run = [&](int scans) {
    for (int i = 0; i < scans; ++i, t += 100) {
      sB.tick(t);
      sE.tick(t);
      sW.tick(t);
      mE.tick(t);
      mW.tick(t);
      cp.tick(t);
    }
  };
  run(20);
  CHECK(frame(cp, {{FunctionClass::Signal, "2", Word::Right}}) == ControlResult::Applied);
  run(10);
  CHECK(mE.displayed() == SignalIndication::Clear);
  CHECK(frame(cp, {{FunctionClass::Signal, "4", Word::Left}}) == ControlResult::VitalIgnored);
  run(10);
  CHECK(mW.displayed() == SignalIndication::Stop);
}

SCENARIO("gap M44 a train seen when the signal dropped time-locks even if it backs away before STOP is proven") {
  SpikeRig s;
  s.m2R.setConfirmMs(1000);
  s.run(2000);
  s.s1A.shunt(true);
  s.code(Word::Normal, Word::Right);
  s.run(2000);
  CHECK(s.m2R.state().indication == SignalIndication::Clear);
  s.code(kNone, Word::Stop);
  s.step();
  s.s1A.shunt(false);  // the train backs out of the approach while the heads still show CLEAR
  s.run(2000);
  CHECK(s.mast2.provenStop());
  CHECK(s.cp.findRoute("MAIN")->state() == Route::State::Timing);
  CHECK(!s.movable());
}

SCENARIO("gap M04 a signal coded while its own route time-locks is unsafe") {
  SpikeRig s;
  s.s1A.shunt(true);
  s.code(Word::Normal, Word::Right);
  s.run(500);
  s.code(kNone, Word::Stop);
  s.run(1000);
  CHECK(s.cp.timeLocking(s.sig2));
  CHECK(s.code(kNone, Word::Right) == ControlResult::VitalIgnored);
}

SCENARIO("gap M31 no command reaches an OFFLINE or FAULTED turnout") {
  const StandInHealth hs[] = {StandInHealth::Offline, StandInHealth::Faulted};
  for (StandInHealth h : hs) {
    SpikeRig s;
    s.run(1000);
    CHECK(s.code(Word::Reverse, kNone) == ControlResult::Applied);
    s.t1.setHealth(h);  // between the frame and the scan that would drive it
    s.run(2000);
    CHECK(s.t1.commands() == 0);
    s.t1.setHealth(StandInHealth::Ok);
    s.run(2000);
    CHECK(s.t1.commands() == 0);  // re-admitted where it stands
    CHECK(s.sw.provenAt(Position::Normal));
  }
}

SCENARIO("gap M30 TIME is shown while a dropped route waits for proof of STOP") {
  SpikeRig s;
  s.s1A.shunt(true);
  s.code(Word::Normal, Word::Right);
  s.run(500);
  s.m2R.setHealth(StandInHealth::Unavailable);
  s.step();
  CHECK(s.cp.findRoute("MAIN")->state() == Route::State::Releasing);
  engine::Indications ind = s.cp.indications();
  CHECK(!ind.is(FunctionClass::Signal, "2", Word::Right));
  CHECK(ind.is(FunctionClass::Signal, "2", Word::Time));
}

SCENARIO("gap M05 a STOP lost in flight is sent again when the mast is re-admitted") {
  SpikeRig s;
  s.code(Word::Normal, Word::Right);
  s.run(500);
  CHECK(s.m2R.displayed() == SignalIndication::Clear);
  s.code(kNone, Word::Stop);
  s.step();  // the STOP is sent
  s.m2R.setHealth(StandInHealth::Unavailable);  // and lost with the link
  s.run(1000);
  CHECK(s.m2R.displayed() == SignalIndication::Clear);
  s.m2R.setHealth(StandInHealth::Ok);
  s.run(1000);
  CHECK(s.m2R.displayed() == SignalIndication::Stop);
}

int main() { return codeline::scenario::run(); }

// ---- Restart with a train inside the plant (M0-M3 review, owner ruling) ---------
// Nothing remembers that the train accepted a signal, so a lined route with a
// train over its switches takes up its route locking again and holds it until
// its circuits are clear.

namespace {

// Power-on with 829 as the stand-in starts (normal), unlike boot().
void restart(CorporalPlant& p) {
  p.tick(p.t);
  p.step();
  p.run(31000);  // any restart time locking runs out
}

}  // namespace

SCENARIO("restart with a train on 829T1 and the route lined holds lock 827 until the train is clear of the plant") {
  CorporalPlant p;
  p.s829T1.shunt(true);  // a westward train past 830WAB, 827 still ahead of it
  restart(p);
  CHECK(p.sw829.provenAt(Position::Normal));
  CHECK(p.cp.findRoute("830W")->state() == Route::State::Traversing);
  CHECK(!p.sw827.WLR());
  CHECK(frame(p.cp, {k829N, k827Release}) == ControlResult::VitalIgnored);
  p.s827T1.shunt(true);  // the train moves on to 827T1
  p.run(500);
  p.s829T1.shunt(false);
  p.run(500);
  CHECK(frame(p.cp, {k829N, k827Release}) == ControlResult::VitalIgnored);
  p.s827T1.shunt(false);  // and leaves the plant
  p.run(500);
  CHECK(p.cp.findRoute("830W")->state() == Route::State::Idle);
  CHECK(p.cp.findRoute("828E")->state() == Route::State::Idle);
  CHECK(frame(p.cp, {k829N, k827Release}) == ControlResult::Applied);
  p.run(500);
  CHECK(p.lock827.releaseLit());
}

SCENARIO("restart with a train on 829T1 and 829 reversed leaves lock 827 free: no route over 827 is lined") {
  CorporalPlant p;
  p.s829T1.shunt(true);
  settle(p);  // boot() finds 829 reversed
  CHECK(p.cp.findRoute("830E")->state() == Route::State::Traversing);
  CHECK(p.cp.findRoute("830W")->state() == Route::State::Idle);
  CHECK(frame(p.cp, {k829R, k827Release}) == ControlResult::Applied);
}

SCENARIO("restart with a train waiting at 830WAB on 832T does not hold the plant: the signal clears after time locking") {
  CorporalPlant p;
  p.s832T.shunt(true);
  restart(p);
  CHECK(p.cp.findRoute("830W")->state() == Route::State::Idle);
  CHECK(p.cp.findRoute("828E")->state() == Route::State::Idle);
  clear830W(p);
}

// ---- Proof of STOP is a report after the STOP command (M0-M3 review, owner ruling) ----

SCENARIO("a cancel before the mast confirms proceed holds the route until the mast answers the STOP") {
  SpikeRig r;
  r.m2R.setConfirmMs(3000);  // slow heads
  CHECK(r.code(Word::Normal, Word::Right) == ControlResult::Applied);
  r.run(400);  // the proceed command is out and the heads are moving; the last report is still STOP
  CHECK(r.m2R.state().indication == SignalIndication::Stop);
  CHECK(r.code(kNone, Word::Stop) == ControlResult::Applied);
  r.run(1000);
  CHECK(!r.mast2.provenStop());  // the old STOP report proves nothing
  CHECK(r.cp.findRoute("MAIN")->state() == Route::State::Releasing);
  CHECK(!r.movable());
  CHECK(r.code(Word::Reverse, kNone) == ControlResult::VitalIgnored);
  r.run(4000);  // the heads settle at STOP and the mast says so, though its state never changed
  CHECK(r.mast2.provenStop());
  CHECK(r.cp.findRoute("MAIN")->state() == Route::State::Idle);  // approach vacant: no time locking
  CHECK(r.movable());
}
