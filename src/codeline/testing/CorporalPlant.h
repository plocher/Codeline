// CP Corporal built by hand over stand-in appliances, from
// tools/spcoast.layout.json (interlockings.Corporal) and
// docs/Walkthrough.md section 1. Until a loader exists, this file is where
// the plant is written down; it is not derived.
//
// Track (west to the left):
//   MT1 west end -- 828EA> -- [827 hand switch, electric lock 827] --
//     827T1 -- IRJ -- 829T1 -- [829 power switch, points east] -- <830WAB -- 832T (single track east)
//   MT2 west end -- 830EAB> -- 829T1 (reverse leg of 829)
//   Beet Loader siding -- <828WA (dwarf) -- [827D derail] -- 827 reverse
//
// Routes (no intermediate home signal between 829 and 827, so a move over
// both sections needs both signal levers, Walkthrough section 1):
//   830W  830WAB, 830 LEFT + 828 LEFT: 829 N, 827 N (locked), 827D N (derailing);
//         clears 829T1 827T1; approach in rear 832T; next signal at Christopher.
//   830E  830EAB, 830 RIGHT: 829 R; clears 829T1 832T; next signal 832EA.
//   828E  828EA, 828 RIGHT + 830 RIGHT: 827 N (locked), 827D N, 829 N;
//         clears 827T1 829T1 832T; next signal 832EA.
// Author choices: 832T (the single-track block toward 832EA) is in the
// eastbound routes' clears() because this slice has no APB/traffic locking;
// 830E and 828E have no approach circuit in this plant (it lies beyond
// Corporal), so they are always treated as approached; 828WA has no route
// (the siding move is a hand move under the lock release); 827 and 827D are
// hand switches the engine never commands.
#pragma once

#include <cstdint>

#include "codeline/engine/Engine.h"
#include "codeline/testing/EngineStandIns.h"

namespace codeline::testing {

struct CorporalPlant {
  // Appliances.
  StandInSensor s829T1, s827T1, s832T;
  StandInTurnout t829, t827, t827D;
  StandInLock lock827;
  StandInMast m830WAB, m830EAB, m828WA, m828EA;
  engine::ConstantFact christopherWest{false};  // next westbound signal: STOP (Walkthrough step 6)
  engine::ConstantFact signal832EA{false};

  engine::Plant cp{"Corporal"};
  engine::TrackCircuit& tc829T1 = cp.addTrackCircuit("829T1", s829T1);
  engine::TrackCircuit& tc827T1 = cp.addTrackCircuit("827T1", s827T1);
  engine::TrackCircuit& tc832T = cp.addTrackCircuit("832T", s832T);
  engine::Switch& sw829 = cp.addPowerSwitch("829", t829, &tc829T1);
  engine::Switch& sw827 = cp.addHandSwitch("827", t827, &tc827T1);
  engine::Switch& sw827D = cp.addHandSwitch("827D", t827D, &tc827T1);
  engine::ElectricLock& el827 = cp.addElectricLock("827", lock827, {&sw827, &sw827D});
  engine::SignalLever& sig828 = cp.addSignalLever("828");
  engine::SignalLever& sig830 = cp.addSignalLever("830");
  engine::SignalMast& mast830WAB = cp.addMast("830WAB", m830WAB);
  engine::SignalMast& mast830EAB = cp.addMast("830EAB", m830EAB);
  engine::SignalMast& mast828WA = cp.addMast("828WA", m828WA);
  engine::SignalMast& mast828EA = cp.addMast("828EA", m828EA);
  uint32_t t;

  explicit CorporalPlant(uint32_t start_ms = 1000) : t(start_ms) {
    using engine::Direction;
    using engine::Position;
    cp.addMaintainerCall("1MC");
    cp.addRoute("830W")
        .shows(mast830WAB, SignalIndication::Clear)
        .governedBy(sig830, Direction::Left)
        .alsoRequires(sig828, Direction::Left)
        .aligns(sw829, Position::Normal, &tc829T1)
        .aligns(sw827, Position::Normal, &tc827T1)
        .aligns(sw827D, Position::Normal, &tc827T1)
        .clears({&tc829T1, &tc827T1})
        .approachLockedBy(tc832T)
        .nextSignal(christopherWest);
    cp.addRoute("830E")
        .shows(mast830EAB, SignalIndication::Clear)
        .governedBy(sig830, Direction::Right)
        .aligns(sw829, Position::Reverse, &tc829T1)
        .clears({&tc829T1, &tc832T})
        .nextSignal(signal832EA);
    cp.addRoute("828E")
        .shows(mast828EA, SignalIndication::Clear)
        .governedBy(sig828, Direction::Right)
        .alsoRequires(sig830, Direction::Right)
        .aligns(sw827, Position::Normal, &tc827T1)
        .aligns(sw827D, Position::Normal, &tc827T1)
        .aligns(sw829, Position::Normal, &tc829T1)
        .clears({&tc827T1, &tc829T1, &tc832T})
        .nextSignal(signal832EA);
    sig828.setTimeLockingMs(30000);  // the Corporal station file's time locking
    sig830.setTimeLockingMs(30000);
    t829.setTravelMs(1000);
    m830WAB.setConfirmMs(300);
    m830EAB.setConfirmMs(300);
    m828EA.setConfirmMs(300);
  }

  // Power-on of everything at t, with 829 found reversed.
  void boot() {
    t829.handThrow(TurnoutState::Reverse);
    tick(t);  // power-on: every appliance reports state, the plant asks for health
    step();   // the attributes arrive: every appliance is admitted
  }

  void tick(uint32_t now) {
    s829T1.tick(now);
    s827T1.tick(now);
    s832T.tick(now);
    t829.tick(now);
    t827.tick(now);
    t827D.tick(now);
    lock827.tick(now);
    m830WAB.tick(now);
    m830EAB.tick(now);
    m828WA.tick(now);
    m828EA.tick(now);
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
};

}  // namespace codeline::testing
