// A switch (or derail) as vital logic sees it: one Appliance:TURNOUT.
//
// Harvested from FieldUnit src/Switch.h with the spike's change 2 made real:
// - the engine never invents MOVING from its own command; it learns IN_TRANSIT
//   (or nothing, if the command was lost) by sampling state();
// - the reported position starts Unknown, so nothing is in correspondence
//   before the appliance has reported (spike scenario S5);
// - correspondence (KR) compares the report with the engine's own demand.
//
// Two kinds:
// - Power: the engine commands it (setAlignment) on an accepted control.
// - Hand: the engine never commands it. A crew throws it by hand while its
//   electric lock is released (827, and the derail 827D, at CP Corporal).
//   It is in correspondence whenever it is proven at either end, and a route
//   over it also requires its electric lock proven locked.
//
// Crossovers and the FieldUnit "dependent derail" pairing are not ported: CP
// Corporal has neither (its derail 827D is an independent appliance, docs/
// appliances/README.Appliances.md, Appliance:TURNOUT invariant 4).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "codeline/appliance/ITurnout.h"
#include "codeline/core/TickClock.h"
#include "codeline/engine/ApplianceLink.h"
#include "codeline/engine/TrackCircuit.h"
#include "codeline/engine/Types.h"

namespace codeline::engine {

class ElectricLock;

class Switch {
public:
  enum class Kind : uint8_t { Power, Hand };

  // `bind` false: a detached switch that never observes the turnout (a
  // configuration the plant refused).
  Switch(std::string_view name, Kind kind, ITurnout& turnout, const TickClock& clock, TrackCircuit* os,
         bool bind = true)
      : name_(name), kind_(kind), turnout_(turnout), link_(turnout, bind), os_(os), travel_(clock) {}

  Switch(const Switch&) = delete;
  Switch& operator=(const Switch&) = delete;

  const std::string& name() const { return name_; }
  Kind kind() const { return kind_; }
  TrackCircuit* osCircuit() const { return os_; }
  ElectricLock* electricLock() const { return elock_; }

  // How long the points may take to reach the demand before a signal demand
  // waiting on them (README.CodeLine section 5.1 rule 1) is dropped.
  void setTravelTimeoutMs(uint32_t ms) { travel_timeout_ms_ = ms; }

  // ---- vital reads ---------------------------------------------------------
  Position reported() const { return reported_; }
  Position demand() const { return demand_; }  // Unknown: no demand (hand switch, or none yet)
  bool healthy() const { return healthy_; }

  // Informative (debug channel); no vital logic reads these.
  Health applianceHealth() const { return link_.health.health(); }
  bool timedOut() const { return timed_out_; }  // the demand was not reached within the travel timeout
  uint32_t travelTimeoutMs() const { return travel_timeout_ms_; }
  uint32_t travelElapsedMs() const { return travel_; }

  // KR. Power: reported equals the demand at a locked end. Hand: at a locked end.
  bool inCorrespondence() const {
    if (!atEnd(reported_)) return false;
    return kind_ == Kind::Hand || reported_ == demand_;
  }
  bool provenAt(Position p) const { return inCorrespondence() && reported_ == p; }

  uint8_t locks() const { return locks_; }
  bool WLR() const { return locks_ == 0; }

  // A demand the points have not yet reached, within the travel timeout.
  // Once the timeout has passed it stays passed until the next demand (the
  // timer itself wraps after 2^32 ms).
  bool settling() const {
    return kind_ == Kind::Power && healthy_ && atEnd(demand_) && reported_ != demand_ && !timed_out_;
  }

  // ---- scan ----------------------------------------------------------------

  // Ingress, once per scan. On admission (first healthy sample since start,
  // or since the appliance was unhealthy) a power switch forgets any demand
  // it held, adopts the proven physical position (none, if the points are
  // at neither end) and sends nothing: switches stay in their last locked
  // position after a restart (README.CodeLine section 5.1 rule 3) and are
  // re-admitted "using their /state" (Walkthrough section 2). Never an
  // unrequested throw. A power switch with no demand adopts the points as
  // soon as they are proven at an end.
  void sample(uint32_t now_ms) {
    const bool healthy = admit(link_.health, turnout_, now_ms);
    if (!healthy) {
      healthy_ = false;
      reported_ = Position::Substituted;
      return;
    }
    const Position p = fromState(turnout_.state());
    if (kind_ == Kind::Power && (!healthy_ || demand_ == Position::Unknown)) {
      demand_ = atEnd(p) ? p : Position::Unknown;
      last_sent_ = demand_;
      timed_out_ = false;
    }
    healthy_ = true;
    reported_ = p;
    if (kind_ == Kind::Power && atEnd(demand_) && reported_ != demand_ &&
        static_cast<uint32_t>(travel_) >= travel_timeout_ms_)
      timed_out_ = true;
  }

  void clearLocks() { locks_ = 0; }
  void addLock(uint8_t bits) { locks_ |= bits; }

  // An accepted control demands a position. The plant has already checked
  // WLR for a change against the last scan's locks; drive() checks it again
  // against this scan's. Nothing is sent before drive(), and never to an
  // unhealthy switch: a demand made while it is unhealthy is forgotten on
  // re-admission (sample()).
  void demand(Position p) {
    if (kind_ != Kind::Power || !atEnd(p) || p == demand_) return;
    demand_ = p;
    travel_ = 0;
    timed_out_ = false;
  }

  // Egress, once per scan, after this scan's locks are built: one one-way
  // command per change of demand, and only while WLR holds now. A demand
  // this scan's locks forbid (a train reached the OS between the frame and
  // this scan) is dropped, back to the last command sent. Nothing is sent
  // to an unhealthy appliance ("withholds operational /cmd", Walkthrough
  // section 2). True if a demand was dropped (so the plant can say so).
  bool drive() {
    if (kind_ != Kind::Power || !healthy_ || !atEnd(demand_) || demand_ == last_sent_) return false;
    if (!WLR()) {
      demand_ = last_sent_;
      return true;
    }
    turnout_.setAlignment(demand_ == Position::Normal ? TurnoutCommand::Normal : TurnoutCommand::Reverse);
    last_sent_ = demand_;
    return false;
  }

private:
  friend class Plant;  // sets elock_ at configuration

  static bool atEnd(Position p) { return p == Position::Normal || p == Position::Reverse; }

  static Position fromState(TurnoutState s) {
    switch (s) {
      case TurnoutState::Normal: return Position::Normal;
      case TurnoutState::Reverse: return Position::Reverse;
      case TurnoutState::InTransit: return Position::InTransit;
    }
    return Position::InTransit;
  }

  std::string name_;
  Kind kind_;
  ITurnout& turnout_;
  TurnoutLink link_;
  TrackCircuit* os_;
  ElectricLock* elock_ = nullptr;

  bool healthy_ = false;
  Position reported_ = Position::Unknown;
  Position demand_ = Position::Unknown;
  Position last_sent_ = Position::Unknown;
  uint8_t locks_ = 0;
  ElapsedMillis travel_;
  uint32_t travel_timeout_ms_ = 2000;  // layout default (docs/Codeline.md section 4)
  bool timed_out_ = false;  // latched: the demand was not reached within the timeout
};

}  // namespace codeline::engine
