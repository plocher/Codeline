// The interlocking plant: configuration, the vital scan, and the office side.
//
// Harvested from FieldUnit src/InterlockingPlant.h and the InterlockingEngine
// of src/ControlTable.h, rebuilt over the Smart Appliance Interface:
//
//   tick(now_ms) = sample -> evaluate -> drive
//
//   sample    one snapshot of every bound appliance (state() plus health),
//             with fail-safe substitution; nothing else reads an appliance
//   evaluate  route states, mast demands, stored signal demands, locks
//   drive     one-way commands for what changed
//
// Time enters only through tick(now_ms); every timer is an ElapsedMillis
// on the plant's TickClock, so the scan is correct across uint32_t rollover.
// There is no millis() fallback.
//
// The plant binds appliances by reference at configuration. It never ticks
// them: whoever owns the appliances ticks them (before the plant, so the
// scan sees the latest reports). Configure everything before the first
// tick(); the first tick freezes the configuration. Configuration after it
// is refused: each add* returns a detached object that is not part of the
// plant and does not bind its appliance, a route setter changes nothing,
// and configurationRefused() turns true for good.
//
// Rules the docs leave open, decided the most restrictive way, are marked
// "Author choice" below.
#pragma once

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "codeline/core/TickClock.h"
#include "codeline/engine/ElectricLock.h"
#include "codeline/engine/Fact.h"
#include "codeline/engine/MaintainerCall.h"
#include "codeline/engine/Office.h"
#include "codeline/engine/Route.h"
#include "codeline/engine/SignalLever.h"
#include "codeline/engine/SignalMast.h"
#include "codeline/engine/Switch.h"
#include "codeline/engine/TrackCircuit.h"
#include "codeline/engine/Types.h"

namespace codeline::engine {

class Plant {
public:
  explicit Plant(std::string_view name) : name_(name) {}

  Plant(const Plant&) = delete;
  Plant& operator=(const Plant&) = delete;

  const std::string& name() const { return name_; }

  // Some configuration arrived after the first tick and was refused.
  bool configurationRefused() const {
    if (refused_) return true;
    for (const Route& r : routes_)
      if (r.refused_) return true;
    return false;
  }

  // ---- configuration ------------------------------------------------------

  TrackCircuit& addTrackCircuit(std::string_view name, ISensor& sensor) {
    if (refuse()) return refused_circuits_.emplace_back(name, sensor, false);
    return circuits_.emplace_back(name, sensor);
  }

  // A switch the engine commands. `os` detector-locks it.
  Switch& addPowerSwitch(std::string_view name, ITurnout& turnout, TrackCircuit* os = nullptr) {
    if (refuse()) return refused_switches_.emplace_back(name, Switch::Kind::Power, turnout, clock_, os, false);
    return switches_.emplace_back(name, Switch::Kind::Power, turnout, clock_, os);
  }

  // A switch or derail the engine only reads (thrown by hand under an
  // electric lock).
  Switch& addHandSwitch(std::string_view name, ITurnout& turnout, TrackCircuit* os = nullptr) {
    if (refuse()) return refused_switches_.emplace_back(name, Switch::Kind::Hand, turnout, clock_, os, false);
    return switches_.emplace_back(name, Switch::Kind::Hand, turnout, clock_, os);
  }

  // An electric lock over hand switches. `name` is the lock function's
  // appliance on the code line ("827").
  ElectricLock& addElectricLock(std::string_view name, IControl& control, std::initializer_list<Switch*> locked) {
    if (refuse()) return refused_locks_.emplace_back(name, control, clock_, false);
    ElectricLock& l = locks_.emplace_back(name, control, clock_);
    for (Switch* sw : locked) {
      l.switches_.push_back(sw);
      sw->elock_ = &l;
    }
    return l;
  }

  SignalLever& addSignalLever(std::string_view name) {
    if (refuse()) return refused_levers_.emplace_back(name);
    return levers_.emplace_back(name);
  }

  SignalMast& addMast(std::string_view name, IMast& mast) {
    if (refuse()) return refused_masts_.emplace_back(name, mast, false);
    return masts_.emplace_back(name, mast);
  }

  // A maintainer call lighting one Appliance:LAMP (MaintainerCall.h). Non-vital:
  // no route reads it. Its indication is the lamp's own report.
  void addMaintainerCall(std::string_view name, ILamp& lamp) {
    if (refuse()) return;
    calls_.emplace_back(name, lamp);
  }

  // A maintainer call with no lamp in the plant: its indication repeats the
  // stored control.
  void addMaintainerCall(std::string_view name) {
    if (refuse()) return;
    calls_.emplace_back(name);
  }

  // A field contact shown to the office as a lamp (a hot box alarm). Non-vital:
  // no route reads it. Lit unless the sensor is healthy and proves it open, so
  // an alarm that cannot be read shows as an alarm.
  void addContact(std::string_view name, ISensor& sensor) {
    if (refuse()) return;
    contacts_.emplace_back(name, sensor);
  }

  // A field lamp of the chart with no source in the plant yet: always dark.
  void addUnboundLamp(std::string_view name) {
    if (refuse()) return;
    unbound_lamps_.emplace_back(name);
  }

  Route& addRoute(std::string_view name) {
    if (refuse()) {
      Route& r = refused_routes_.emplace_back(name, clock_);
      r.frozen_ = true;
      return r;
    }
    return routes_.emplace_back(name, clock_);
  }

  // ---- lookups ---------------------------------------------------------------

  TrackCircuit* findTrackCircuit(std::string_view n) { return find(circuits_, n); }
  Switch* findSwitch(std::string_view n) { return find(switches_, n); }
  ElectricLock* findLock(std::string_view n) { return find(locks_, n); }
  SignalLever* findLever(std::string_view n) { return find(levers_, n); }
  SignalMast* findMast(std::string_view n) { return find(masts_, n); }
  Route* findRoute(std::string_view n) { return find(routes_, n); }

  // ---- read-only introspection (the debug channel; nothing vital reads it) -----

  const std::deque<TrackCircuit>& trackCircuits() const { return circuits_; }
  const std::deque<TrackCircuit>& contacts() const { return contacts_; }
  const std::deque<Switch>& switches() const { return switches_; }
  const std::deque<ElectricLock>& electricLocks() const { return locks_; }
  const std::deque<SignalLever>& levers() const { return levers_; }
  const std::deque<SignalMast>& masts() const { return masts_; }
  const std::deque<MaintainerCall>& maintainerCalls() const { return calls_; }
  const std::deque<Route>& routes() const { return routes_; }

  // Why the last apply() answered VitalIgnored (the first hazard found, in
  // plain words); empty after any other answer.
  const std::string& lastRefusal() const { return refusal_; }

  // What the scans since the last call dropped, and why: a stored signal
  // demand that cleared nothing, a cleared signal put back to STOP, a switch
  // demand the locks forbade. Recorded where the engine decides it. At most
  // kMaxNotes are kept if nobody takes them.
  std::vector<std::string> takeNotes() {
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
  }

  // What locks a switch as of the last scan, in words; empty: free (WLR up).
  // It reads the same terms as evaluate() step 5, which builds the lock bits.
  std::string lockedBy(const Switch& sw) const {
    std::vector<std::string> by;
    auto add = [&by](std::string s) {
      for (const std::string& b : by)
        if (b == s) return;
      by.push_back(std::move(s));
    };
    if (sw.osCircuit() != nullptr && !sw.osCircuit()->clear())
      add("detector locking (" + sw.osCircuit()->name() + " not proven clear)");
    for (const Route& r : routes_) {
      if (!r.uses(&sw)) continue;
      switch (r.state_) {
        case Route::State::Cleared: add("route " + r.name() + " (cleared)"); break;
        case Route::State::Releasing: add("route " + r.name() + " (awaiting proof of STOP)"); break;
        case Route::State::Timing: add("route " + r.name() + " (time locking)"); break;
        case Route::State::Traversing:
          for (size_t i = 0; i < r.switches_.size(); ++i)
            if (r.switches_[i].sw == &sw && !r.released_[i]) add("route " + r.name() + " (train on the route)");
          break;
        case Route::State::Idle: break;
      }
      if (!provenStop(r))
        add("mast " + (r.mast_ ? r.mast_->name() : std::string("?")) + " not proven at STOP");
    }
    std::string out;
    for (const std::string& b : by) out += (out.empty() ? "" : ", ") + b;
    return out;
  }

  // ---- the vital scan --------------------------------------------------------

  void tick(uint32_t now_ms) {
    clock_.advanceTo(now_ms);
    if (!finalized_) finalize();
    sample(now_ms);
    evaluate();
    drive();
  }

  // ---- the office side -------------------------------------------------------

  ControlTransaction newControls() const { return ControlTransaction(*this); }

  // Applies one atomic control transaction (README.CodeLine 5.1, 5.3). Call
  // between scans; its effects reach the appliances at the next tick().
  //
  // Author choice: before the first tick the plant has proven nothing and
  // acts on no frame at all, not even its non-vital controls; the frame is
  // reported Malformed (ignored as a whole), the only result that says so.
  // A transaction built against another plant, or against this plant
  // before its configuration grew, is Malformed too.
  ControlResult apply(const ControlTransaction& t) {
    refusal_.clear();
    if (!finalized_ || t.plant_ != this || t.malformed_) return ControlResult::Malformed;
    if (t.switches_.size() != switches_.size() || t.signals_.size() != levers_.size() ||
        t.locks_.size() != locks_.size() || t.calls_.size() != calls_.size())
      return ControlResult::Malformed;
    for (size_t i = 0; i < calls_.size(); ++i) calls_[i].call(t.calls_[i] != 0);  // non-vital, always
    if (!safe(t, refusal_)) return ControlResult::VitalIgnored;

    for (size_t i = 0; i < switches_.size(); ++i) {
      if (t.switches_[i] == ControlTransaction::kNormal) switches_[i].demand(Position::Normal);
      if (t.switches_[i] == ControlTransaction::kReverse) switches_[i].demand(Position::Reverse);
    }
    for (size_t i = 0; i < locks_.size(); ++i) locks_[i].grant(t.locks_[i] != 0);  // a level
    for (size_t i = 0; i < levers_.size(); ++i) {
      switch (t.signals_[i]) {
        case ControlTransaction::kLeft:
        case ControlTransaction::kRight:
          levers_[i].demand(t.signals_[i] == ControlTransaction::kLeft ? Direction::Left : Direction::Right);
          // README.CodeLine 5.1 rule 1: held only while switches of this
          // frame settle.
          awaits_[i].clear();
          for (size_t k = 0; k < switches_.size(); ++k)
            if (t.switches_[k] != 0) awaits_[i].push_back(&switches_[k]);
          break;
        case ControlTransaction::kStop: levers_[i].drop(); break;
        default: break;  // unchanged
      }
    }
    return ControlResult::Applied;
  }

  Indications indications() const {
    Indications ind(*this);
    for (const Switch& sw : switches_) {
      ind.switch_n_.push_back(sw.provenAt(Position::Normal));
      ind.switch_r_.push_back(sw.provenAt(Position::Reverse));
    }
    for (const ElectricLock& l : locks_) {
      ind.lock_released_.push_back(l.releasedIndication());
      ind.lock_request_.push_back(l.requested());
    }
    for (const SignalLever& lever : levers_) {
      const bool gl = provenProceed(lever, Direction::Left);
      const bool gr = provenProceed(lever, Direction::Right);
      ind.signal_l_.push_back(gl);
      ind.signal_r_.push_back(gr);
      ind.signal_t_.push_back(timeLockingIndication(lever, gl, gr));
    }
    for (const MaintainerCall& c : calls_) ind.calls_.push_back(c.lit());
    for (const TrackCircuit& tc : circuits_) ind.occupied_.push_back(!tc.clear());
    for (const TrackCircuit& c : contacts_) ind.lamps_.push_back(!c.clear());
    for (size_t i = 0; i < unbound_lamps_.size(); ++i) ind.lamps_.push_back(0);
    return ind;
  }

  // The stored CALL control of a maintainer call (what the office last asked
  // for), not what its lamp shows: that is the CALL indication.
  bool maintainerCall(std::string_view n) const {
    for (const MaintainerCall& c : calls_)
      if (c.name() == n) return c.called();
    return false;
  }

  // Time locking (approach or time) holds a route of this lever.
  bool timeLocking(const SignalLever& lever) const {
    for (const Route& r : routes_)
      if (r.lever() == &lever && (r.state() == Route::State::Releasing || r.state() == Route::State::Timing))
        return true;
    return false;
  }

private:
  friend class ControlTransaction;
  friend class Indications;

  bool refuse() {
    if (finalized_) refused_ = true;
    return finalized_;
  }

  template <class T>
  static T* find(std::deque<T>& items, std::string_view n) {
    for (T& item : items)
      if (item.name() == n) return &item;
    return nullptr;
  }
  template <class T>
  static int indexOf(const std::deque<T>& items, std::string_view n) {
    for (size_t i = 0; i < items.size(); ++i)
      if (items[i].name() == n) return static_cast<int>(i);
    return -1;
  }
  int callIndex(std::string_view n) const { return indexOf(calls_, n); }

  // Two routes from different masts conflict when they share a switch or a
  // track circuit: opposing and converging moves alike. Each must find the
  // other idle and its mast proven at STOP before it clears (RGKR; spike
  // section 5, defect 1). Routes from one mast never line at once.
  void finalize() {
    finalized_ = true;
    awaits_.assign(levers_.size(), {});
    for (Route& a : routes_) {
      a.frozen_ = true;
      a.released_.assign(a.switches_.size(), 0);
      a.occupied_.assign(a.switches_.size(), 0);
      for (Route& b : routes_) {
        if (&a == &b || a.mast_ == b.mast_) continue;
        bool shared = false;
        for (const Route::SwitchReq& s : b.switches_) shared = shared || a.uses(s.sw);
        for (const TrackCircuit* tc : b.blocks_) shared = shared || a.uses(tc);
        if (shared) a.conflicts_.push_back(&b);
      }
    }
  }

  void sample(uint32_t now_ms) {
    for (TrackCircuit& tc : circuits_) tc.sample(now_ms);
    for (TrackCircuit& c : contacts_) c.sample(now_ms);
    for (Switch& sw : switches_) sw.sample(now_ms);
    for (SignalMast& m : masts_) m.sample(now_ms);
    for (ElectricLock& l : locks_) l.sample(now_ms);
    for (MaintainerCall& c : calls_) c.sample(now_ms);  // non-vital: nothing in evaluate() reads it
    for (Route& r : routes_) r.next_proceed_ = r.next_ != nullptr && r.next_->asserted();
  }

  // ---- evaluate ----------------------------------------------------------------

  // A route without a mast (a configuration error) is never proven at STOP.
  static bool provenStop(const Route& r) { return r.mast_ != nullptr && r.mast_->provenStop(); }

  // A route over a hand switch needs its electric lock proven locked.
  static bool switchProven(const Route::SwitchReq& s) {
    if (!s.sw->provenAt(s.pos)) return false;
    return s.sw->electricLock() == nullptr || s.sw->electricLock()->provenLocked();
  }

  // Says no, and why if someone asked. The words are built only then.
  template <class Text>
  static bool no(std::string* why, Text&& text) {
    if (why != nullptr) *why = text();
    return false;
  }

  static std::string circuitHazard(const TrackCircuit& tc) {
    return "track circuit " + tc.name() + (tc.healthy() ? " is occupied" : " is not proven clear (unhealthy)");
  }
  static std::string conflictHazard(const Route& c) {
    if (c.state_ != Route::State::Idle)
      return "conflicting route " + c.name() + " is " + Route::stateName(c.state_);
    return "mast " + (c.mast_ ? c.mast_->name() : std::string("?")) + " of conflicting route " + c.name() +
           " is not proven at STOP";
  }
  static std::string lockHazard(const ElectricLock& l) {
    const char* how = !l.healthy()      ? "unhealthy"
                      : l.open()        ? "open"
                      : l.granted()     ? "release granted"
                      : l.withdrawing() ? "release not yet proven withdrawn"
                                        : "release never withdrawn";
    return "lock " + l.name() + " is not proven locked (" + how + ")";
  }
  static std::string switchHazard(const Route::SwitchReq& s) {
    const Switch& sw = *s.sw;
    if (sw.provenAt(s.pos)) return lockHazard(*sw.electricLock());
    const std::string late =
        sw.timedOut() ? " within " + std::to_string(sw.travelTimeoutMs()) + " ms" : std::string();
    if (sw.reported() == s.pos)  // at the wanted end, but its stored demand is the other end
      return "switch " + sw.name() + " reports " + positionName(s.pos) + " but is out of correspondence: its demand is " +
             positionName(sw.demand()) + (late.empty() ? "" : ", which it did not reach" + late);
    return "switch " + sw.name() + " is not proven " + positionName(s.pos) + " (it reports " +
           positionName(sw.reported()) + (late.empty() ? "" : ", it did not settle" + late) + ")";
  }

  // `why`, if given, gets the first hazard found when the answer is no.
  bool clearable(const Route& r, std::string* why = nullptr) const {
    if (r.mast_ == nullptr || r.levers_.empty()) return no(why, [] { return std::string("it has no mast or no lever"); });
    if (!r.mast_->healthy()) return no(why, [&r] { return "mast " + r.mast_->name() + " is not healthy"; });
    for (const Route::LeverReq& l : r.levers_)
      if (l.lever->active() != l.dir)
        return no(why, [&l] { return "signal lever " + l.lever->name() + " is not " + directionName(l.dir); });
    if (timeLocking(*r.lever()))  // ASR down: no route of this lever clears
      return no(why, [&r] { return "time locking is running on signal lever " + r.lever()->name(); });
    for (const Route::SwitchReq& s : r.switches_)
      if (!switchProven(s)) return no(why, [&s] { return switchHazard(s); });
    for (const TrackCircuit* b : r.blocks_)
      if (!b->clear()) return no(why, [b] { return circuitHazard(*b); });
    for (const Route* c : r.conflicts_)
      if (c->state_ != Route::State::Idle || !provenStop(*c)) return no(why, [c] { return conflictHazard(*c); });
    return true;
  }

  // The words are built only if the note is kept.
  template <class Text>
  void note(Text&& text) {
    if (notes_.size() < kMaxNotes) notes_.push_back(text());
  }

  // Why a stored LEFT/RIGHT cleared nothing: each route that needs it, and its first hazard.
  std::string whyNothingCleared(const SignalLever& lever) const {
    std::string out;
    for (const Route& r : routes_) {
      if (!r.requires(&lever, lever.active())) continue;
      std::string why;
      if (clearable(r, &why)) why = std::string("it is ") + Route::stateName(r.state_);
      out += (out.empty() ? "route " : "; route ") + r.name() + ": " + why;
    }
    return out.empty() ? "no route needs it" : out;
  }

  SignalIndication indicationOf(const Route& r) const {
    return r.next_proceed_ ? r.ceiling_ : capAtApproach(r.ceiling_);
  }

  void dropLevers(Route& r) {
    for (Route::LeverReq& l : r.levers_) l.lever->drop();
  }

  void enterTraversing(Route& r) {
    r.state_ = Route::State::Traversing;
    dropLevers(r);  // the train accepted the signal: the stick drops (5.1 rule 2)
    for (size_t i = 0; i < r.switches_.size(); ++i) {
      const TrackCircuit* rel = r.switches_[i].releasing;
      r.released_[i] = 0;
      r.occupied_[i] = rel != nullptr && rel->occupied();
    }
  }

  void evaluate() {
    // 1. Routes already holding something.
    for (Route& r : routes_) {
      switch (r.state_) {
        case Route::State::Traversing:
          // Sectional release (FieldUnit InterlockingEngine::evaluate): a
          // section releases only on proven occupancy followed by proven
          // vacancy. An unhealthy circuit is neither: it holds the lock.
          for (size_t i = 0; i < r.switches_.size(); ++i) {
            const TrackCircuit* rel = r.switches_[i].releasing;
            if (rel == nullptr) continue;
            if (!r.occupied_[i] && rel->occupied()) r.occupied_[i] = 1;
            else if (r.occupied_[i] && rel->clear()) r.released_[i] = 1;
          }
          if (r.blocksClear()) r.state_ = Route::State::Idle;
          break;
        case Route::State::Releasing:
          // Approach locking: wait for proof of STOP, then time it if a train
          // approaches (now, or when the signal dropped).
          if (provenStop(r)) {
            if (r.timed_ || r.approachOccupied()) {
              r.state_ = Route::State::Timing;
              r.ter_ = 0;
            } else {
              r.state_ = Route::State::Idle;
            }
          }
          break;
        case Route::State::Timing:
          if (r.lever() != nullptr && static_cast<uint32_t>(r.ter_) >= r.lever()->timeLockingMs())
            r.state_ = Route::State::Idle;  // a route with no lever (a configuration error) never releases
          break;
        case Route::State::Cleared:
          // Only a healthy entrance circuit proven occupied is a train
          // accepting the signal. An unhealthy one makes the route not
          // clearable: the signal drops with approach locking.
          if (!r.blocks_.empty() && r.blocks_.front()->occupied()) {
            enterTraversing(r);
          } else if (!clearable(r)) {
            // The signal drops for any reason but a train accepting it:
            // approach locking (spike section 5, defect 2). Author choice:
            // the stored demand drops too, so the signal does not re-clear by
            // itself when the hazard goes away (spike section 6, item 3).
            note([this, &r] {  // asked again for the words, before anything changes: the same answer
              std::string why;
              clearable(r, &why);
              return "signal " + (r.mast_ ? r.mast_->name() : std::string("?")) + " put to STOP, route " + r.name() +
                     " dropped: " + why;
            });
            r.state_ = Route::State::Releasing;
            r.timed_ = r.approachOccupied();
            dropLevers(r);
          }
          break;
        case Route::State::Idle:
          break;
      }
    }

    // 2. Idle routes that may clear, in declaration order: the first of two
    //    conflicting routes wins the scan, the second sees it not idle.
    for (Route& r : routes_) {
      if (r.state_ == Route::State::Idle && clearable(r)) r.state_ = Route::State::Cleared;
    }

    // 3. Mast demands: most permissive across the cleared routes of a mast,
    //    least permissive along each (ceiling, DR). Everything else STOP.
    for (SignalMast& m : masts_) {
      SignalIndication best = SignalIndication::Stop;
      for (const Route& r : routes_)
        if (r.mast_ == &m && r.state_ == Route::State::Cleared) best = mostPermissive(best, indicationOf(r));
      m.demand(best);
    }

    // 4. Stored signal demands (README.CodeLine 5.1 rule 1): a demand that
    //    cannot clear now is held only while a switch its own frame
    //    commanded is still settling, then dropped.
    //    (The drops are made after the loop only so that each note is worded
    //    with every lever as the frame left it; no lever's fate reads another.)
    std::vector<SignalLever*> dropped;
    for (size_t i = 0; i < levers_.size(); ++i) {
      SignalLever& lever = levers_[i];
      if (!lever.pending()) continue;
      bool cleared = false;
      for (const Route& r : routes_)
        cleared = cleared || (r.state_ == Route::State::Cleared && r.requires(&lever, lever.active()));
      bool settling = false;
      for (const Switch* sw : awaits_[i]) settling = settling || sw->settling();
      if (cleared) lever.fulfilled();
      else if (!settling) {
        note([this, &lever] {
          return std::string("signal ") + lever.name() + " " + directionName(lever.active()) +
                 " demand dropped, nothing cleared: " + whyNothingCleared(lever);
        });
        dropped.push_back(&lever);
      }
    }
    for (SignalLever* lever : dropped) lever->drop();

    // 5. Switch locks, rebuilt from the route states every scan.
    for (Switch& sw : switches_) {
      sw.clearLocks();
      if (sw.osCircuit() != nullptr && !sw.osCircuit()->clear()) sw.addLock(lock::kDetector);
    }
    for (Route& r : routes_) {
      for (size_t i = 0; i < r.switches_.size(); ++i) {
        Switch* sw = r.switches_[i].sw;
        switch (r.state_) {
          case Route::State::Cleared:
          case Route::State::Releasing: sw->addLock(lock::kRoute); break;
          case Route::State::Timing: sw->addLock(lock::kTime); break;
          case Route::State::Traversing:
            if (!r.released_[i]) sw->addLock(lock::kRoute);
            break;
          case Route::State::Idle: break;
        }
        // A mast not proven at STOP may still display proceed over these
        // switches (spike section 4, changes 6-7). Author choice: whatever
        // the switches' positions.
        if (!provenStop(r)) sw->addLock(lock::kRoute);
      }
    }
  }

  void drive() {
    for (Switch& sw : switches_) {
      const Position wanted = sw.demand();
      if (sw.drive())
        note([this, &sw, wanted] {
          return "switch " + sw.name() + " demand " + positionName(wanted) + " dropped: locked by " + lockedBy(sw);
        });
    }
    for (SignalMast& m : masts_) m.drive();
    for (ElectricLock& l : locks_) l.drive();
    for (MaintainerCall& c : calls_) c.drive();
  }

  // ---- control transaction safety (README.CodeLine 5.3) -------------------------

  // The position a switch will be demanded at if this frame is applied.
  Position effective(const ControlTransaction& t, const Switch& sw) const {
    const size_t i = indexOfSwitch(sw);
    if (sw.kind() == Switch::Kind::Hand) return sw.reported();
    if (t.switches_[i] == ControlTransaction::kNormal) return Position::Normal;
    if (t.switches_[i] == ControlTransaction::kReverse) return Position::Reverse;
    return sw.demand();
  }
  size_t indexOfSwitch(const Switch& sw) const {
    for (size_t i = 0; i < switches_.size(); ++i)
      if (&switches_[i] == &sw) return i;
    return 0;
  }
  size_t indexOfLock(const ElectricLock& l) const {
    for (size_t i = 0; i < locks_.size(); ++i)
      if (&locks_[i] == &l) return i;
    return 0;
  }

  // The route would line under the frame's switch positions.
  bool lined(const ControlTransaction& t, const Route& r) const {
    for (const Route::SwitchReq& s : r.switches_)
      if (effective(t, *s.sw) != s.pos) return false;
    return true;
  }

  // A frame is unsafe when any vital control in it would violate locking.
  // Author choices (most restrictive): any occupied circuit of a lined route
  // (not only the OS) makes a LEFT/RIGHT unsafe; two signals of one frame
  // whose lined routes conflict make it unsafe (neither wins by declaration
  // order); a LEFT/RIGHT over an electric lock that is not proven locked
  // now, or that the frame itself releases, is unsafe, so a frame cannot
  // re-lock and clear over the lock at once; a STOP never makes a frame
  // unsafe, but an unsafe frame ignores its STOPs too, as 5.3 says.
  // `why` gets the first hazard found when the answer is no.
  bool safe(const ControlTransaction& t, std::string& why) const {
    // Switches: a change of demand needs WLR.
    for (size_t i = 0; i < switches_.size(); ++i) {
      const Switch& sw = switches_[i];
      const Position e = effective(t, sw);
      if (sw.kind() == Switch::Kind::Power && e != sw.demand() && !sw.WLR())
        return no(&why, [&] {
          return "switch " + sw.name() + " to " + positionName(e) + ": it is locked by " + lockedBy(sw);
        });
    }
    // Signals: LEFT or RIGHT.
    std::vector<const Route*> wanted;
    for (size_t li = 0; li < levers_.size(); ++li) {
      const uint8_t w = t.signals_[li];
      if (w != ControlTransaction::kLeft && w != ControlTransaction::kRight) continue;
      const Direction d = w == ControlTransaction::kLeft ? Direction::Left : Direction::Right;
      const SignalLever& lever = levers_[li];
      auto what = [&lever, d] { return "signal " + lever.name() + " " + directionName(d); };
      if (timeLocking(lever)) return no(&why, [&] { return what() + ": time locking is running on that lever"; });
      for (const Route& r : routes_) {
        if (!r.requires(&levers_[li], d) || !lined(t, r)) continue;
        wanted.push_back(&r);
        auto over = [&what, &r] { return what() + ", route " + r.name() + ": "; };
        for (const TrackCircuit* b : r.blocks_)
          if (!b->clear()) return no(&why, [&] { return over() + circuitHazard(*b); });
        for (const Route* c : r.conflicts_)
          if (c->state_ != Route::State::Idle || !provenStop(*c)) return no(&why, [&] { return over() + conflictHazard(*c); });
        for (const Route::SwitchReq& s : r.switches_) {
          const ElectricLock* l = s.sw->electricLock();
          if (l == nullptr) continue;
          if (!l->provenLocked()) return no(&why, [&] { return over() + lockHazard(*l); });
          if (t.locks_[indexOfLock(*l)])
            return no(&why, [&] { return over() + "the same frame releases lock " + l->name(); });
        }
      }
    }
    for (const Route* a : wanted)
      for (const Route* c : a->conflicts_)
        for (const Route* b : wanted)
          if (b == c)
            return no(&why, [&] {
              return "signals of this frame conflict: route " + a->name() + " and route " + b->name();
            });
    // Locks: a new release needs nothing lined, locked, or unproven at STOP
    // over its switches (FieldUnit how-to 03, narrowed to the locked
    // switches so the Walkthrough's parallel move on MT2 stays possible).
    for (size_t k = 0; k < locks_.size(); ++k) {
      if (!t.locks_[k] || locks_[k].granted()) continue;
      auto what = [this, k] { return "lock " + locks_[k].name() + " release: "; };
      for (const Switch* sw : locks_[k].switches()) {
        if (!sw->WLR())
          return no(&why, [&] { return what() + "switch " + sw->name() + " is locked by " + lockedBy(*sw); });
        for (const Route& r : routes_) {
          if (!r.uses(sw)) continue;
          if (r.state_ != Route::State::Idle)
            return no(&why, [&] {
              return what() + "route " + r.name() + " over switch " + sw->name() + " is " + Route::stateName(r.state_);
            });
          if (!provenStop(r))
            return no(&why, [&] {
              return what() + "mast " + (r.mast_ ? r.mast_->name() : std::string("?")) + " of route " + r.name() +
                     " is not proven at STOP";
            });
        }
      }
    }
    return true;
  }

  // ---- indications ------------------------------------------------------------------

  // signal/LEFT or RIGHT: a healthy mast of a route that needs this lever
  // that way reports a proceed, whatever the route's state (a mast stuck at
  // proceed behind an idle route is still showing proceed). Paired through
  // the routes, never by declaration index (spike section 5, defect 4).
  bool provenProceed(const SignalLever& lever, Direction d) const {
    for (const Route& r : routes_) {
      if (r.mast_ == nullptr || !r.requires(&lever, d)) continue;
      if (r.mast_->confirmed() != SignalIndication::Stop) return true;
    }
    return false;
  }

  // signal/TIME (README.CodeLine section 3): a route governed by this lever
  // is running its TER, or awaiting proof of STOP. Never while LEFT or RIGHT
  // is asserted on the lever (TIME with LEFT or RIGHT is an invalid group,
  // 5.2): a lever co-required by another route whose mast still shows
  // proceed keeps its GK, and its TIME waits until that proceed is gone.
  bool timeLockingIndication(const SignalLever& lever, bool gl, bool gr) const {
    if (gl || gr) return false;
    for (const Route& r : routes_) {
      if (r.lever() != &lever) continue;
      if (r.state_ == Route::State::Timing || r.state_ == Route::State::Releasing) return true;
    }
    return false;
  }

  std::string name_;
  TickClock clock_;
  bool finalized_ = false;
  bool refused_ = false;  // configuration after the first tick
  std::deque<TrackCircuit> circuits_;
  std::deque<TrackCircuit> contacts_;       // lamp indications, in this order...
  std::vector<std::string> unbound_lamps_;  // ...then these
  std::deque<Switch> switches_;
  std::deque<ElectricLock> locks_;
  std::deque<SignalLever> levers_;
  std::deque<SignalMast> masts_;
  std::deque<Route> routes_;
  std::deque<MaintainerCall> calls_;
  std::vector<std::vector<const Switch*>> awaits_;  // per lever: the switches its last LEFT/RIGHT frame commanded

  // Informative only (the debug channel): nothing in the scan reads these back.
  static constexpr size_t kMaxNotes = 64;
  std::string refusal_;
  std::vector<std::string> notes_;

  // Refused configuration: detached, never sampled, evaluated or driven.
  std::deque<TrackCircuit> refused_circuits_;
  std::deque<Switch> refused_switches_;
  std::deque<ElectricLock> refused_locks_;
  std::deque<SignalLever> refused_levers_;
  std::deque<SignalMast> refused_masts_;
  std::deque<Route> refused_routes_;
};

// ---- ControlTransaction / Indications -------------------------------------------------

inline ControlTransaction::ControlTransaction(const Plant& plant)
    : plant_(&plant),
      switches_(plant.switches_.size(), 0),
      signals_(plant.levers_.size(), 0),
      locks_(plant.locks_.size(), 0),
      calls_(plant.calls_.size(), 0) {}

inline void ControlTransaction::assertFunction(FunctionClass cls, std::string_view appliance, Word word) {
  const Plant& p = *plant_;
  auto add = [this](std::vector<uint8_t>& v, int i, uint8_t bit) {
    if (i < 0 || v[static_cast<size_t>(i)] != 0) {
      malformed_ = true;  // unknown function, or a second word of one group
      return;
    }
    v[static_cast<size_t>(i)] |= bit;
  };
  switch (cls) {
    case FunctionClass::Switch: {
      int i = Plant::indexOf(p.switches_, appliance);
      if (i >= 0 && p.switches_[static_cast<size_t>(i)].kind() != Switch::Kind::Power) i = -1;
      if (word == Word::Normal) add(switches_, i, kNormal);
      else if (word == Word::Reverse) add(switches_, i, kReverse);
      else malformed_ = true;
      return;
    }
    case FunctionClass::Signal: {
      const int i = Plant::indexOf(p.levers_, appliance);
      if (word == Word::Left) add(signals_, i, kLeft);
      else if (word == Word::Right) add(signals_, i, kRight);
      else if (word == Word::Stop) add(signals_, i, kStop);
      else malformed_ = true;
      return;
    }
    case FunctionClass::Lock:
      if (word == Word::Release) add(locks_, Plant::indexOf(p.locks_, appliance), 1);
      else malformed_ = true;
      return;
    case FunctionClass::MaintainerCall:
      if (word == Word::Call) add(calls_, p.callIndex(appliance), 1);
      else malformed_ = true;
      return;
    default:
      malformed_ = true;  // track_circuit and lamp have no controls
      return;
  }
}

inline bool Indications::get(FunctionClass cls, std::string_view appliance, Word word, bool& asserted) const {
  const Plant& p = *plant_;
  auto pick = [&asserted](const std::vector<uint8_t>& v, int i) {
    if (i < 0) return false;
    asserted = v[static_cast<size_t>(i)] != 0;
    return true;
  };
  switch (cls) {
    case FunctionClass::Switch: {
      const int i = Plant::indexOf(p.switches_, appliance);
      if (word == Word::Normal) return pick(switch_n_, i);
      if (word == Word::Reverse) return pick(switch_r_, i);
      return false;
    }
    case FunctionClass::Lock: {
      const int i = Plant::indexOf(p.locks_, appliance);
      if (word == Word::Released) return pick(lock_released_, i);
      if (word == Word::Request) return pick(lock_request_, i);
      return false;
    }
    case FunctionClass::Signal: {
      const int i = Plant::indexOf(p.levers_, appliance);
      if (word == Word::Left) return pick(signal_l_, i);
      if (word == Word::Right) return pick(signal_r_, i);
      if (word == Word::Time) return pick(signal_t_, i);
      return false;
    }
    case FunctionClass::MaintainerCall:
      return word == Word::Call && pick(calls_, p.callIndex(appliance));
    case FunctionClass::TrackCircuit:
      return word == Word::Occupied && pick(occupied_, Plant::indexOf(p.circuits_, appliance));
    case FunctionClass::Lamp: {
      if (word != Word::Lit) return false;
      int i = Plant::indexOf(p.contacts_, appliance);
      if (i < 0) {
        for (size_t u = 0; u < p.unbound_lamps_.size(); ++u)
          if (p.unbound_lamps_[u] == appliance) i = static_cast<int>(p.contacts_.size() + u);
      }
      return pick(lamps_, i);
    }
  }
  return false;
}

}  // namespace codeline::engine
