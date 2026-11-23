// One row of the control table: a route from a signal mast, and its locking
// state. Harvested from FieldUnit src/ControlTable.h (Route), with these
// changes:
//
// - The circuit in rear of the signal used for approach locking
//   (approachLockedBy) and the term that reduces the aspect (nextSignal, the
//   DR term) are two different inputs. FieldUnit used one circuit for both
//   (spike section 5, defect 3).
// - A route may require more than its own lever: at CP Corporal the
//   westbound route from 830WAB onto MT1 crosses both sections and needs
//   signal lever 828 LEFT as well as 830 LEFT (Walkthrough section 1).
// - The route owns its approach / time locking (Releasing, Timing). It is
//   entered whenever a cleared route drops for any reason other than a
//   train accepting it, and the TER starts only once the mast is proven at
//   STOP (spike section 4, change 5; section 5, defect 2).
// - Engine return and automatic (APB) routes are not ported.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codeline/core/TickClock.h"
#include "codeline/engine/Fact.h"
#include "codeline/engine/SignalLever.h"
#include "codeline/engine/SignalMast.h"
#include "codeline/engine/Switch.h"
#include "codeline/engine/TrackCircuit.h"
#include "codeline/engine/Types.h"

namespace codeline::engine {

class Route {
public:
  enum class State : uint8_t {
    Idle,       // nothing lined; no route locking
    Cleared,    // signal cleared: route locked
    Traversing, // a train accepted the signal: sectional release
    Releasing,  // signal dropped without a train accepting it: route locked until the mast is proven at STOP
    Timing,     // proven at STOP with a train approaching (or approach unknown): TER running
  };

  static const char* stateName(State s) {
    switch (s) {
      case State::Idle: return "Idle";
      case State::Cleared: return "Cleared";
      case State::Traversing: return "Traversing";
      case State::Releasing: return "Releasing";
      case State::Timing: return "Timing";
    }
    return "?";
  }

  struct SwitchReq {
    Switch* sw;
    Position pos;
    TrackCircuit* releasing;  // its section releases when this circuit is occupied and then clear
  };

  struct LeverReq {
    SignalLever* lever;
    Direction dir;
  };

  Route(std::string_view name, const TickClock& clock) : name_(name), ter_(clock) {}

  Route(const Route&) = delete;
  Route& operator=(const Route&) = delete;

  // ---- configuration (fluent) -------------------------------------------
  // Once the plant is finalized (its first tick) the route is frozen: every
  // setter is a no-op that marks the configuration refused
  // (Plant::configurationRefused).

  // The mast this route clears, and the best indication it may show.
  Route& shows(SignalMast& mast, SignalIndication ceiling) {
    if (refuse()) return *this;
    mast_ = &mast;
    ceiling_ = ceiling;
    return *this;
  }
  // The lever whose signal this is. Its time locking (TEK) is this route's.
  Route& governedBy(SignalLever& lever, Direction dir) {
    if (refuse()) return *this;
    levers_.insert(levers_.begin(), LeverReq{&lever, dir});
    return *this;
  }
  // A further lever that must also be held in a direction.
  Route& alsoRequires(SignalLever& lever, Direction dir) {
    if (refuse()) return *this;
    levers_.push_back(LeverReq{&lever, dir});
    return *this;
  }
  Route& aligns(Switch& sw, Position pos, TrackCircuit* releasing = nullptr) {
    if (refuse()) return *this;
    switches_.push_back(SwitchReq{&sw, pos, releasing});
    return *this;
  }
  // Every circuit the route needs vacant. The first is the entrance: a train
  // occupying it has accepted the signal.
  Route& clears(std::initializer_list<TrackCircuit*> circuits) {
    if (refuse()) return *this;
    for (TrackCircuit* tc : circuits) blocks_.push_back(tc);
    return *this;
  }
  // The circuit in rear of the signal: occupied means a train is approaching.
  // A route without one is always treated as approached (most restrictive).
  Route& approachLockedBy(TrackCircuit& tc) {
    if (refuse()) return *this;
    approach_ = &tc;
    return *this;
  }
  // The DR term: whether the next signal ahead shows proceed. A route without
  // one shows no better than approach (most restrictive).
  Route& nextSignal(const Fact& proceed) {
    if (refuse()) return *this;
    next_ = &proceed;
    return *this;
  }

  // ---- reads ----------------------------------------------------------------

  const std::string& name() const { return name_; }
  SignalMast* mast() const { return mast_; }
  SignalIndication ceiling() const { return ceiling_; }
  SignalLever* lever() const { return levers_.empty() ? nullptr : levers_.front().lever; }
  Direction direction() const { return levers_.empty() ? Direction::None : levers_.front().dir; }
  const std::vector<LeverReq>& levers() const { return levers_; }
  const std::vector<SwitchReq>& switches() const { return switches_; }
  const std::vector<TrackCircuit*>& blocks() const { return blocks_; }
  TrackCircuit* approachCircuit() const { return approach_; }
  const std::vector<Route*>& conflicts() const { return conflicts_; }
  State state() const { return state_; }

  bool requires(const SignalLever* lever, Direction dir) const {
    for (const LeverReq& l : levers_)
      if (l.lever == lever && l.dir == dir) return true;
    return false;
  }
  bool uses(const Switch* sw) const {
    for (const SwitchReq& s : switches_)
      if (s.sw == sw) return true;
    return false;
  }
  bool uses(const TrackCircuit* tc) const {
    for (const TrackCircuit* b : blocks_)
      if (b == tc) return true;
    return false;
  }
  bool blocksClear() const {
    for (const TrackCircuit* b : blocks_)
      if (!b->clear()) return false;
    return true;
  }
  bool approachOccupied() const { return approach_ == nullptr || !approach_->clear(); }
  // Every switch proven where this route needs it.
  bool aligned() const {
    for (const SwitchReq& s : switches_)
      if (!s.sw->provenAt(s.pos)) return false;
    return true;
  }
  uint32_t terElapsedMs() const { return ter_; }
  // While traversing: the section of switches()[i] has released. Informative.
  bool sectionReleased(size_t i) const { return i < released_.size() && released_[i] != 0; }

private:
  friend class Plant;

  bool refuse() {
    if (frozen_) refused_ = true;
    return frozen_;
  }

  std::string name_;
  SignalMast* mast_ = nullptr;
  SignalIndication ceiling_ = SignalIndication::Stop;
  std::vector<LeverReq> levers_;
  std::vector<SwitchReq> switches_;
  std::vector<TrackCircuit*> blocks_;
  TrackCircuit* approach_ = nullptr;
  const Fact* next_ = nullptr;
  std::vector<Route*> conflicts_;  // filled when the plant is finalized
  bool frozen_ = false;            // set when the plant is finalized
  bool refused_ = false;           // a setter was called while frozen

  // Restart (README.CodeLine 5.1 rule 3): every route starts as if its signal
  // had just dropped, so time locking runs where a train approaches.
  State state_ = State::Releasing;
  bool timed_ = false;         // a train approached when the signal dropped
  bool next_proceed_ = false;  // DR snapshot for this scan
  std::vector<uint8_t> released_;   // per switch: section released while traversing
  std::vector<uint8_t> occupied_;   // per switch: section seen occupied while traversing
  ElapsedMillis ter_;
};

}  // namespace codeline::engine
