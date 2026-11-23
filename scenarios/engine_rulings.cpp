// Owner rulings folded into the engine scenarios:
// 1. Cluster:MAST STATE: a mast holds its prior confirmed indication while
//    its heads move, for every change, and a failed head faults instead of
//    reporting STOP (docs/clusters/README.Clusters.md, MAST STATE).
// 2. Electric lock on loss of control: no appliance watchdog; fail-safe
//    stays with vital logic (docs/Codeline.md, fail-safe substitution).
// 3. Co-required levers (docs/codeline/README.CodeLine.md section 3).
// Also docs/Codeline.md section 4 step 5: TER defaults to 15 000 ms.
#include <cstdint>
#include <initializer_list>
#include <memory>

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
const Fn k827Release{FunctionClass::Lock, "827", Word::Release};
const Fn k828L{FunctionClass::Signal, "828", Word::Left};
const Fn k830L{FunctionClass::Signal, "830", Word::Left};
const Fn k830Stop{FunctionClass::Signal, "830", Word::Stop};

bool is(const engine::Plant& p, FunctionClass c, const char* a, Word w) { return p.indications().is(c, a, w); }
bool gk(const engine::Plant& p, const char* lever) {
  return is(p, FunctionClass::Signal, lever, Word::Left) || is(p, FunctionClass::Signal, lever, Word::Right);
}
bool tek(const engine::Plant& p, const char* lever) { return is(p, FunctionClass::Signal, lever, Word::Time); }
bool wlk(const engine::Plant& p) { return is(p, FunctionClass::Lock, "827", Word::Released); }

void settle(CorporalPlant& p) {
  p.boot();
  p.run(31000);
}

// Lock 827 released at the dispatcher's request, lamp lit.
void release827(CorporalPlant& p) {
  CHECK(frame(p.cp, {k829N, k827Release}) == ControlResult::Applied);
  p.run(3000);
  CHECK(p.lock827.releaseLit());
  CHECK(wlk(p.cp));
}

}  // namespace

// ---- 1. Mast state rule -------------------------------------------------------------

SCENARIO("ruling 1 a mast moving to STOP holds its proceed: GK and route locks last exactly as long, TER starts at the report of red") {
  SpikeRig s;
  s.m2R.setConfirmMs(2000);
  s.s1A.shunt(true);  // a train approaches: the cancel time-locks
  s.code(Word::Normal, Word::Right);
  s.run(3000);
  CHECK(s.m2R.state().indication == SignalIndication::Clear);
  s.code(kNone, Word::Stop);
  uint32_t proof = 0;
  bool ok = true;
  while (s.t < 1000000 && proof == 0) {
    s.step();
    const bool proceed = s.m2R.state().indication != SignalIndication::Stop;
    if (proceed) {
      ok = ok && gk(s.cp, "2") && !tek(s.cp, "2") && !s.movable();
      ok = ok && s.cp.findRoute("MAIN")->state() == Route::State::Releasing;
    } else {
      proof = s.t;
      ok = ok && s.mast2.provenStop() && !gk(s.cp, "2") && tek(s.cp, "2");
      ok = ok && s.cp.findRoute("MAIN")->state() == Route::State::Timing;
      ok = ok && s.cp.findRoute("MAIN")->terElapsedMs() == 0;  // started at this report
    }
  }
  CHECK(ok);
  CHECK(proof != 0);
  s.run(proof + 29900 - s.t);
  CHECK(!s.movable());
  s.run(200);
  CHECK(s.movable());
}

SCENARIO("ruling 1 a proceed-to-proceed change never shows STOP and never drops the route") {
  SpikeRig s;
  s.m2R.setConfirmMs(1500);
  s.code(Word::Normal, Word::Right);
  s.run(2500);
  CHECK(s.m2R.state().indication == SignalIndication::Clear);
  s.nextMain.set(false);  // the next signal drops: CLEAR -> APPROACH
  bool ok = true;
  for (int i = 0; i < 30; ++i) {
    s.step();
    ok = ok && s.m2R.state().indication != SignalIndication::Stop;
    ok = ok && s.cp.findRoute("MAIN")->state() == Route::State::Cleared;
    ok = ok && gk(s.cp, "2") && !tek(s.cp, "2") && !s.movable();
  }
  CHECK(ok);
  CHECK(s.mast2.demanded() == SignalIndication::Approach);
  CHECK(s.m2R.state().indication == SignalIndication::Approach);
}

SCENARIO("ruling 1 a mast whose head fails faults and is never proven at STOP") {
  SpikeRig s;
  s.code(Word::Normal, Word::Right);
  s.run(500);
  CHECK(s.m2R.state().indication == SignalIndication::Clear);
  s.m2R.failHead();
  s.run(120000);
  CHECK(!s.mast2.provenStop());
  CHECK(s.cp.findRoute("MAIN")->state() == Route::State::Releasing);
  CHECK(!s.movable());
  CHECK(!gk(s.cp, "2"));  // indication use: STOP
  CHECK(tek(s.cp, "2"));  // TEK stays lit: the dispatcher calls the maintainer
  CHECK(s.code(Word::Reverse, kNone) == ControlResult::VitalIgnored);
}

// ---- 2. Electric lock on loss of control ----------------------------------------------

SCENARIO("ruling 2a the code line goes down after a release: the release stays as last accepted") {
  CorporalPlant p;
  settle(p);
  release827(p);
  const uint32_t lamps = p.lock827.commands();
  p.run(600000);  // ten minutes without a frame
  CHECK(p.el827.granted());
  CHECK(p.lock827.releaseLit());
  CHECK(p.lock827.commands() == lamps);  // nothing in the field touched the release
  CHECK(p.lock827.open(true));            // the crew uses the lock
  p.t827.handThrow(TurnoutState::Reverse);
  p.run(1000);
  p.t827.handThrow(TurnoutState::Normal);
  p.lock827.open(false);
  p.run(1000);
  CHECK(wlk(p.cp));
  CHECK(p.m830WAB.displayed() == SignalIndication::Stop);
  CHECK(p.lock827.releaseLit());
  // The code line returns: the next frame states the lock again.
  CHECK(frame(p.cp, {k829N}) == ControlResult::Applied);
  p.run(500);
  CHECK(!p.lock827.releaseLit());
  p.run(1500);
  CHECK(p.el827.provenLocked());
}

SCENARIO("ruling 2b the lock's link drops while release is granted: not proven locked; the grant is re-sent on return") {
  CorporalPlant p;
  settle(p);
  release827(p);
  const uint32_t lamps = p.lock827.commands();
  p.lock827.setHealth(StandInHealth::Unavailable);
  p.run(5000);
  CHECK(!p.el827.provenLocked());
  CHECK(wlk(p.cp));
  CHECK(p.lock827.releaseLit());  // the output stays as last driven
  CHECK(frame(p.cp, {k829N, k827Release, k828L, k830L}) == ControlResult::VitalIgnored);
  p.lock827.setHealth(StandInHealth::Ok);
  p.run(1000);
  CHECK(p.lock827.commands() == lamps + 1);  // the current frame, release granted, re-sent
  CHECK(p.lock827.releaseLit());
  CHECK(!p.el827.provenLocked());
  CHECK(wlk(p.cp));
}

SCENARIO("ruling 2b the grant withdrawn while the lock's link is down: withdrawal sent on return, then re-proven") {
  CorporalPlant p;
  settle(p);
  release827(p);
  p.lock827.setHealth(StandInHealth::Unavailable);
  p.step();
  CHECK(frame(p.cp, {k829N}) == ControlResult::Applied);  // re-lock while the link is down
  p.run(5000);
  CHECK(p.lock827.releaseLit());  // unreachable: the output stays as last driven
  CHECK(wlk(p.cp));
  p.lock827.setHealth(StandInHealth::Ok);
  p.run(400);  // re-admitted, lamp off sent, applied after the latency
  CHECK(!p.lock827.releaseLit());
  CHECK(!p.el827.provenLocked());  // not before the withdrawal is proven
  CHECK(frame(p.cp, {k829N, k828L, k830L}) == ControlResult::VitalIgnored);
  p.run(1200);
  CHECK(p.el827.provenLocked());
  CHECK(!wlk(p.cp));
  CHECK(frame(p.cp, {k829N, k828L, k830L}) == ControlResult::Applied);
}

namespace {

// A small plant over a hand switch H under lock L, for restarts: the field
// logic is rebuilt over the same appliances.
struct LockBench {
  StandInSensor sT;
  StandInTurnout tH;
  StandInLock lock;
  StandInMast m;
  engine::ConstantFact go{true};
  std::unique_ptr<engine::Plant> cp;
  engine::ElectricLock* el = nullptr;
  uint32_t t = 1000;

  void build() {
    cp.reset();
    cp = std::make_unique<engine::Plant>("BENCH");
    engine::TrackCircuit& T = cp->addTrackCircuit("T", sT);
    engine::Switch& H = cp->addHandSwitch("H", tH, &T);
    el = &cp->addElectricLock("L", lock, {&H});
    engine::SignalLever& lever = cp->addSignalLever("2");
    engine::SignalMast& mast = cp->addMast("2R", m);
    cp->addRoute("R").shows(mast, SignalIndication::Clear).governedBy(lever, Direction::Right)
        .aligns(H, Position::Normal, &T).clears({&T}).nextSignal(go);
  }
  void step() {
    t += 100;
    sT.tick(t);
    tH.tick(t);
    lock.tick(t);
    m.tick(t);
    cp->tick(t);
  }
  void run(uint32_t ms) {
    for (uint32_t end = t + ms; static_cast<int32_t>(end - t) > 0;) step();
  }
  ControlResult release() { return frame(*cp, {{FunctionClass::Lock, "L", Word::Release}}); }
};

}  // namespace

SCENARIO("ruling 2c a field restart while release is granted sends the withdrawal and re-proves the lock") {
  LockBench b;
  b.build();
  b.run(32000);  // restart time locking (no approach circuit) runs out
  CHECK(b.release() == ControlResult::Applied);
  b.run(1000);
  CHECK(b.lock.releaseLit());
  b.build();  // restart: every demand cleared; on reconnect the appliances replay retained state
  for (auto h : {StandInHealth::Unavailable, StandInHealth::Ok}) {
    b.sT.setHealth(h);
    b.tH.setHealth(h);
    b.lock.setHealth(h);
    b.m.setHealth(h);
  }
  b.run(200);
  CHECK(!b.el->granted());
  CHECK(!b.el->provenLocked());
  b.run(300);
  CHECK(!b.lock.releaseLit());  // the lamp off actually reached the appliance
  CHECK(!b.el->provenLocked());
  b.run(1000);
  CHECK(b.el->provenLocked());
}

SCENARIO("ruling 2c a lock unreachable at restart gets the withdrawal when it returns") {
  LockBench b;
  b.build();
  b.run(32000);  // restart time locking (no approach circuit) runs out
  CHECK(b.release() == ControlResult::Applied);
  b.run(1000);
  CHECK(b.lock.releaseLit());
  b.lock.setHealth(StandInHealth::Unavailable);
  b.build();  // restart while the lock is away
  for (auto h : {StandInHealth::Unavailable, StandInHealth::Ok}) {
    b.sT.setHealth(h);
    b.tH.setHealth(h);
    b.m.setHealth(h);
  }
  b.run(5000);
  CHECK(b.lock.releaseLit());  // unreachable: as last driven
  CHECK(!b.el->provenLocked());
  CHECK(b.cp->indications().is(FunctionClass::Lock, "L", Word::Released));
  b.lock.setHealth(StandInHealth::Ok);
  b.run(500);
  CHECK(!b.lock.releaseLit());
  CHECK(!b.el->provenLocked());
  b.run(1000);
  CHECK(b.el->provenLocked());
  CHECK(!b.cp->indications().is(FunctionClass::Lock, "L", Word::Released));
}

// ---- 3. Co-required levers ---------------------------------------------------------------

SCENARIO("ruling 3 830WAB's proceed asserts 830WGK and 828WGK together; TEK only on 830, never with GK") {
  CorporalPlant p;
  p.m830WAB.setConfirmMs(2000);
  settle(p);
  p.s832T.shunt(true);  // a train approaches 830WAB: the cancel time-locks
  CHECK(frame(p.cp, {k829N, k828L, k830L}) == ControlResult::Applied);
  bool ok = true, sawGK = false, sawHeld = false, sawTEK = false;
  auto check = [&] {
    const bool g830 = gk(p.cp, "830"), g828 = gk(p.cp, "828");
    ok = ok && g830 == g828;
    ok = ok && !tek(p.cp, "828");
    ok = ok && !(tek(p.cp, "830") && g830);
    const bool proceed = p.m830WAB.state().indication != SignalIndication::Stop;
    ok = ok && g830 == proceed;
    sawGK = sawGK || g830;
    sawTEK = sawTEK || tek(p.cp, "830");
  };
  for (int i = 0; i < 50; ++i) {
    p.step();
    check();
  }
  CHECK(p.m830WAB.state().indication == SignalIndication::Approach);
  CHECK(frame(p.cp, {k829N, k828L, k830Stop}) == ControlResult::Applied);  // the Walkthrough cancel
  for (int i = 0; i < 400; ++i) {
    p.step();
    check();
    if (p.m830WAB.state().indication != SignalIndication::Stop)
      sawHeld = sawHeld || p.cp.findRoute("830W")->state() == Route::State::Releasing;
  }
  CHECK(ok);
  CHECK(sawGK);
  CHECK(sawHeld);  // the mast held its proceed while the route was releasing
  CHECK(sawTEK);
  CHECK(p.cp.findRoute("830W")->state() == Route::State::Idle);
}

SCENARIO("ruling 3 TIME never shows with LEFT/RIGHT on one lever, even when a co-requiring route's mast still shows proceed") {
  // Lever X governs A; lever Y governs B, which also requires X. A and B
  // share nothing, so both clear at once.
  StandInSensor sA, sA2, sB;
  StandInMast mA, mB;
  engine::ConstantFact go{true};
  engine::Plant cp("P");
  engine::TrackCircuit& A = cp.addTrackCircuit("A", sA);
  engine::TrackCircuit& A2 = cp.addTrackCircuit("A2", sA2);
  engine::TrackCircuit& B = cp.addTrackCircuit("B", sB);
  engine::SignalLever& x = cp.addSignalLever("X");
  engine::SignalLever& y = cp.addSignalLever("Y");
  engine::SignalMast& ma = cp.addMast("MA", mA);
  engine::SignalMast& mb = cp.addMast("MB", mB);
  cp.addRoute("A").shows(ma, SignalIndication::Clear).governedBy(x, Direction::Right).clears({&A, &A2}).nextSignal(go);
  cp.addRoute("B").shows(mb, SignalIndication::Clear).governedBy(y, Direction::Right).alsoRequires(x, Direction::Right)
      .clears({&B}).nextSignal(go);
  mB.setConfirmMs(5000);  // MB's heads are slow
  uint32_t t = 1000;
  bool ok = true, sawTimeX = false;
  auto run = [&](int scans) {
    for (int i = 0; i < scans; ++i) {
      t += 100;
      sA.tick(t);
      sA2.tick(t);
      sB.tick(t);
      mA.tick(t);
      mB.tick(t);
      cp.tick(t);
      const bool gx = gk(cp, "X");
      ok = ok && !(tek(cp, "X") && gx) && !(tek(cp, "Y") && gk(cp, "Y"));
      sawTimeX = sawTimeX || tek(cp, "X");
    }
  };
  run(400);  // restart time locking runs out
  CHECK(frame(cp, {{FunctionClass::Signal, "X", Word::Right}, {FunctionClass::Signal, "Y", Word::Right}}) ==
        ControlResult::Applied);
  run(80);
  CHECK(mA.displayed() == SignalIndication::Clear);
  CHECK(mB.displayed() == SignalIndication::Clear);
  sA2.shunt(true);  // A's block ahead fouled: A drops (time locking: no approach circuit), X drops, B drops
  run(20);          // MA proven at STOP: A times; MB still holds CLEAR
  CHECK(cp.findRoute("A")->state() == Route::State::Timing);
  CHECK(mB.state().indication == SignalIndication::Clear);
  CHECK(gk(cp, "X"));
  run(400);
  CHECK(ok);
  CHECK(sawTimeX);
}

// ---- TER default (docs/Codeline.md section 4 step 5) -------------------------------------

SCENARIO("TER defaults to 15 000 ms") {
  engine::SignalLever lever("2");
  CHECK(lever.timeLockingMs() == 15000);
}

int main() { return codeline::scenario::run(); }
