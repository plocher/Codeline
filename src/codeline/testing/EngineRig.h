// Test plants for the engine scenarios, built by hand over stand-in
// appliances (codeline/testing/EngineStandIns.h). Every appliance is ticked
// before the plant, as a host loop would.
//
// SpikeRig: the M1c spike's plant. One power switch 1 with its OS 1T, an
// approach circuit 1A in rear of signal 2, and two routes from mast 2R:
//   MAIN    2 RIGHT, 1 NORMAL, clears 1T 1M, ceiling CLEAR
//   SIDING  2 RIGHT, 1 REVERSE, clears 1T 1S, ceiling DIVERGING_CLEAR
// a maintainer call 1MC, and, for the opposing-signal scenarios, mast 4L on lever 4:
//   WEST    4 LEFT, 1 NORMAL, clears 1T 1W, ceiling CLEAR
#pragma once

#include <cstdint>

#include "codeline/engine/Engine.h"
#include "codeline/testing/EngineStandIns.h"

namespace codeline::testing {

using engine::ControlResult;
using engine::Direction;
using engine::FunctionClass;
using engine::Position;
using engine::Route;
using engine::Word;

struct SpikeRig {
  StandInSensor s1T, s1A, s1M, s1S, s1W;
  StandInTurnout t1;
  StandInMast m2R, m4L;
  engine::ConstantFact nextMain{true}, nextSide{true}, nextWest{true};

  engine::Plant cp{"CP_M1C"};
  engine::TrackCircuit& os = cp.addTrackCircuit("1T", s1T);
  engine::TrackCircuit& appr = cp.addTrackCircuit("1A", s1A);
  engine::TrackCircuit& main = cp.addTrackCircuit("1M", s1M);
  engine::TrackCircuit& side = cp.addTrackCircuit("1S", s1S);
  engine::TrackCircuit& west = cp.addTrackCircuit("1W", s1W);
  engine::Switch& sw = cp.addPowerSwitch("1", t1, &os);
  engine::SignalLever& sig2 = cp.addSignalLever("2");
  engine::SignalLever& sig4 = cp.addSignalLever("4");
  engine::SignalMast& mast2 = cp.addMast("2R", m2R);
  engine::SignalMast& mast4 = cp.addMast("4L", m4L);
  uint32_t t;

  explicit SpikeRig(uint32_t start_ms = 1000, bool withWest = false) : t(start_ms) {
    cp.addRoute("MAIN")
        .shows(mast2, SignalIndication::Clear)
        .governedBy(sig2, Direction::Right)
        .aligns(sw, Position::Normal, &os)
        .clears({&os, &main})
        .approachLockedBy(appr)
        .nextSignal(nextMain);
    cp.addRoute("SIDING")
        .shows(mast2, SignalIndication::DivergingClear)
        .governedBy(sig2, Direction::Right)
        .aligns(sw, Position::Reverse, &os)
        .clears({&os, &side})
        .approachLockedBy(appr)
        .nextSignal(nextSide);
    if (withWest) {
      cp.addRoute("WEST")
          .shows(mast4, SignalIndication::Clear)
          .governedBy(sig4, Direction::Left)
          .aligns(sw, Position::Normal, &os)
          .clears({&os, &west})
          .approachLockedBy(main)
          .nextSignal(nextWest);
    }
    cp.addMaintainerCall("1MC");
    sig2.setTimeLockingMs(30000);
    sig4.setTimeLockingMs(30000);
    t1.setTravelMs(500);
    tick(t);  // power-on: every appliance reports state, the plant asks for health
    step();   // the attributes arrive: every appliance is admitted
  }

  void tick(uint32_t now) {
    s1T.tick(now);
    s1A.tick(now);
    s1M.tick(now);
    s1S.tick(now);
    s1W.tick(now);
    t1.tick(now);
    m2R.tick(now);
    m4L.tick(now);
    cp.tick(now);
  }
  void step(uint32_t ms = 100) {
    t += ms;
    tick(t);
  }
  void run(uint32_t ms, uint32_t s = 100) {
    const uint32_t end = t + ms;
    while (static_cast<int32_t>(end - t) > 0) step(s);
  }

  // One control frame: switch 1 word (or none), lever 2 word (or none),
  // lever 4 word (or none).
  ControlResult code(Word sw1, Word sig, Word sig4w = Word::Lit) {
    engine::ControlTransaction c = cp.newControls();
    if (sw1 != Word::Lit) c.assertFunction(FunctionClass::Switch, "1", sw1);
    if (sig != Word::Lit) c.assertFunction(FunctionClass::Signal, "2", sig);
    if (sig4w != Word::Lit) c.assertFunction(FunctionClass::Signal, "4", sig4w);
    return cp.apply(c);
  }
  bool movable() const { return sw.WLR(); }
};

// "No function" in SpikeRig::code.
inline constexpr Word kNone = Word::Lit;

}  // namespace codeline::testing
