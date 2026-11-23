// The M1c spike's closed-loop scenarios S1-S5 (docs/research/
// engine-closed-loop-spike.md section 3.2), rerun on the harvested engine
// over stand-in appliances. The spike's three GAPs are now checks.
#include "codeline/testing/EngineRig.h"
#include "scenario.h"

using namespace codeline;
using namespace codeline::testing;

SCENARIO("S1 a mast slow to confirm STOP delays the start of time locking") {
  SpikeRig r;
  r.m2R.setConfirmMs(3000);
  r.s1A.shunt(true);  // a train approaches
  r.run(200);
  CHECK(r.code(Word::Normal, Word::Right) == ControlResult::Applied);
  r.run(4000);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);  // approach in rear no longer downgrades (defect 3)

  const uint32_t t0 = r.t;
  CHECK(r.code(kNone, Word::Stop) == ControlResult::Applied);
  uint32_t confirmedAt = 0;
  bool movableBeforeProof = false;
  while (r.t < t0 + 5000) {
    r.step();
    if (!confirmedAt && r.m2R.state().indication == SignalIndication::Stop) confirmedAt = r.t;
    if (!confirmedAt && r.movable()) movableBeforeProof = true;
  }
  CHECK(confirmedAt >= t0 + 3000);
  CHECK(!movableBeforeProof);
  r.run(t0 + 30500 - r.t);
  CHECK(!r.movable());  // 30.5 s after cancel, 27 s after proof: still time locked
  CHECK(r.code(Word::Reverse, kNone) == ControlResult::VitalIgnored);
  r.step();
  CHECK(r.sw.demand() == Position::Normal);
  r.run(confirmedAt + 30000 + 200 - r.t);
  CHECK(r.movable());
  CHECK(!r.cp.timeLocking(r.sig2));
}

SCENARIO("S2 a jammed turnout never lets the signal clear, and the demand is dropped") {
  SpikeRig r;
  r.t1.setJammed(true);
  CHECK(r.code(Word::Reverse, Word::Right) == ControlResult::Applied);
  bool everProceed = false, everShown = false;
  for (int i = 0; i < 600; ++i) {
    r.step();
    everProceed = everProceed || r.mast2.demanded() != SignalIndication::Stop;
    everShown = everShown || r.m2R.state().indication != SignalIndication::Stop;
  }
  CHECK(!everProceed);
  CHECK(!everShown);
  CHECK(!r.sw.inCorrespondence());
  // README.CodeLine 5.1 rule 1: the signal demand was held only while the
  // switch settled (travel timeout), then dropped.
  CHECK(r.sig2.active() == Direction::None);
  // The office sees no proceed: GK comes from the confirmed mast, not the
  // stick (the spike's S2 GAP).
  engine::Indications ind = r.cp.indications();
  CHECK(!ind.is(FunctionClass::Signal, "2", Word::Right));
  CHECK(!ind.is(FunctionClass::Switch, "1", Word::Reverse));
  CHECK(!ind.is(FunctionClass::Switch, "1", Word::Normal));

  r.t1.setJammed(false);  // the maintainer frees the points
  r.run(1000);
  CHECK(r.sw.provenAt(Position::Reverse));
  CHECK(r.m2R.state().indication == SignalIndication::Stop);  // no re-clear without a new code
  CHECK(r.code(kNone, Word::Right) == ControlResult::Applied);
  r.run(500);
  CHECK(r.m2R.state().indication == SignalIndication::DivergingClear);
}

SCENARIO("S2b a slow healthy turnout: no proceed command before the points confirm") {
  SpikeRig r;
  r.t1.setLatencyMs(300);
  r.t1.setTravelMs(1200);  // slow, but inside the 2 s travel supervision
  const uint32_t t0 = r.t;
  CHECK(r.code(Word::Reverse, Word::Right) == ControlResult::Applied);
  uint32_t arrived = 0, firstProceed = 0;
  while (r.t < t0 + 5000) {
    r.step();
    if (!arrived && r.t1.state() == TurnoutState::Reverse) arrived = r.t;
    if (!firstProceed && r.m2R.commands() > 0 && r.m2R.lastCommand() != SignalIndication::Stop) firstProceed = r.t;
  }
  CHECK(arrived != 0 && firstProceed >= arrived);
  CHECK(r.m2R.state().indication == SignalIndication::DivergingClear);
}

SCENARIO("S3a a turnout going UNAVAILABLE under a clear signal drops it") {
  SpikeRig r;
  CHECK(r.code(Word::Normal, Word::Right) == ControlResult::Applied);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Clear);
  r.t1.setHealth(StandInHealth::Unavailable);
  r.step();
  CHECK(r.sw.reported() == Position::Substituted);
  CHECK(r.mast2.demanded() == SignalIndication::Stop);
  r.run(300);
  CHECK(r.m2R.state().indication == SignalIndication::Stop);
  r.t1.setHealth(StandInHealth::Ok);
  r.run(2000);
  // Author choice: the stored demand dropped with the signal; no re-clear.
  CHECK(r.m2R.state().indication == SignalIndication::Stop);
  CHECK(r.sw.provenAt(Position::Normal));
}

SCENARIO("S3b a mast UNAVAILABLE while lit holds the locks until it is proven at STOP") {
  SpikeRig m;
  m.s1A.shunt(true);
  CHECK(m.code(Word::Normal, Word::Right) == ControlResult::Applied);
  m.run(300);
  CHECK(m.m2R.state().indication == SignalIndication::Clear);
  m.m2R.setHealth(StandInHealth::Unavailable);  // lamps stay lit as last commanded
  m.step();
  CHECK(m.mast2.demanded() == SignalIndication::Stop);
  CHECK(m.mast2.confirmed() == SignalIndication::Stop);  // indication use
  CHECK(!m.mast2.provenStop());                          // locking use
  CHECK(!m.movable());
  m.code(kNone, Word::Stop);
  m.run(60000);
  CHECK(!m.movable());
  CHECK(m.m2R.displayed() == SignalIndication::Clear);  // still lit out there
  const uint32_t back = m.t;
  m.m2R.setHealth(StandInHealth::Ok);
  m.run(500);  // proof of health, then the re-sent STOP after the command latency
  CHECK(m.m2R.state().indication == SignalIndication::Stop);  // STOP re-sent on re-admission
  m.run(back + 29000 - m.t);
  CHECK(!m.movable());
  m.run(back + 31000 - m.t);
  CHECK(m.movable());
}

SCENARIO("S3c a route sensor UNAVAILABLE in front of an approaching train: approach locking holds") {
  SpikeRig s;
  s.s1A.shunt(true);
  CHECK(s.code(Word::Normal, Word::Right) == ControlResult::Applied);
  s.run(300);
  CHECK(s.m2R.state().indication == SignalIndication::Clear);
  s.s1M.setHealth(StandInHealth::Unavailable);
  s.step();
  CHECK(!s.main.clear());
  CHECK(s.mast2.demanded() == SignalIndication::Stop);
  s.run(500);
  CHECK(s.m2R.state().indication == SignalIndication::Stop);
  CHECK(!s.movable());  // the spike's S3c GAP
  CHECK(s.cp.indications().is(FunctionClass::Signal, "2", Word::Time));
  s.s1M.setHealth(StandInHealth::Ok);
  s.run(29000);
  CHECK(!s.movable());
  s.run(1500);
  CHECK(s.movable());
}

SCENARIO("S4 an OS sensor that is UNAVAILABLE, FAULT or OFFLINE reads occupied") {
  SpikeRig r;
  const StandInHealth hs[] = {StandInHealth::Unavailable, StandInHealth::Faulted, StandInHealth::Offline};
  for (StandInHealth h : hs) {
    r.s1T.setHealth(h);
    r.step();
    CHECK(!r.os.clear());
    CHECK((r.sw.locks() & engine::lock::kDetector) != 0);
    CHECK(r.code(Word::Reverse, kNone) == ControlResult::VitalIgnored);
    r.step();
    CHECK(r.sw.demand() == Position::Normal);
    CHECK(r.t1.commands() == 0);
    CHECK(r.cp.indications().is(FunctionClass::TrackCircuit, "1T", Word::Occupied));
    r.s1T.setHealth(StandInHealth::Ok);
    r.step();
    r.step();
  }
  CHECK(r.os.clear());
  CHECK(r.movable());
}

SCENARIO("S4b an OS sensor failing under a clear signal knocks it down for good") {
  SpikeRig q;
  CHECK(q.code(Word::Normal, Word::Right) == ControlResult::Applied);
  q.run(300);
  CHECK(q.m2R.state().indication == SignalIndication::Clear);
  q.s1T.setHealth(StandInHealth::Unavailable);
  q.step();
  CHECK(q.mast2.demanded() == SignalIndication::Stop);
  CHECK(q.sig2.active() == Direction::None);
  q.s1T.setHealth(StandInHealth::Ok);
  q.run(1000);
  CHECK(q.m2R.state().indication == SignalIndication::Stop);
}

SCENARIO("S5 startup: nothing is proven before the appliances report") {
  StandInSensor os;
  StandInTurnout t;
  t.setHealth(StandInHealth::Unavailable);
  engine::Plant cp("CP_START");
  engine::TrackCircuit& tc = cp.addTrackCircuit("1T", os);
  engine::Switch& sw = cp.addPowerSwitch("1", t, &tc);
  CHECK(sw.reported() == Position::Unknown);
  CHECK(!sw.inCorrespondence());  // the spike's S5 GAP
  CHECK(!tc.clear());
  os.tick(1000);
  t.tick(1000);
  cp.tick(1000);
  CHECK(!tc.clear());  // reported, but its health is not yet proven
  os.tick(1100);
  t.tick(1100);
  cp.tick(1100);
  CHECK(sw.reported() == Position::Substituted);
  CHECK(!sw.inCorrespondence());
  CHECK(tc.clear());
  engine::Indications ind = cp.indications();
  CHECK(!ind.is(FunctionClass::Switch, "1", Word::Normal));
  CHECK(!ind.is(FunctionClass::Switch, "1", Word::Reverse));
}

SCENARIO("S5b startup: a turnout found at an end is adopted without moving it") {
  StandInTurnout t;
  t.handThrow(TurnoutState::Reverse);
  engine::Plant cp("CP_START");
  engine::Switch& sw = cp.addPowerSwitch("1", t);
  t.tick(1000);
  cp.tick(1000);
  CHECK(!sw.provenAt(Position::Reverse));  // health not yet proven
  t.tick(1100);
  cp.tick(1100);
  CHECK(sw.provenAt(Position::Reverse));
  CHECK(sw.demand() == Position::Reverse);
  t.tick(1200);
  cp.tick(1200);
  CHECK(t.commands() == 0);
}

int main() { return codeline::scenario::run(); }
