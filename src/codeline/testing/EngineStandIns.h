// Stand-in Smart Appliances for the engine scenarios, adapted from the M1c
// spike (docs/research/engine-closed-loop-spike.md section 1). Each one
// implements the real typed interface (ITurnout, ISensor, IMast, IControl)
// and nothing else: the engine reaches them only through that interface.
// They stand in until SensorCore / MastCore / ControlCore exist; they are
// not those cores.
//
// They are no more forgiving than a remote appliance over MQTT:
// - commands are one-way and take effect inside a later tick(now_ms), once
//   the command latency (default kDefaultLatencyMs) has passed;
// - state() changes, and every report or event (state, attributes, button)
//   is emitted, only inside tick(now_ms). Fault injection with setHealth()
//   is the one exception: it stands for an event that arrived between two
//   scans;
// - exportAttributes() never answers synchronously: the AttributesEvent
//   carrying maintenance.offline and maintenance.faulted (how
//   ApplianceHealth proves health) comes from a later tick(), once the
//   attribute latency has passed (default 0: the next tick);
// - the first tick is power-on: it reports state (onStateChanged);
// - a command, or a request for attributes, sent while UNAVAILABLE or still
//   in flight when the link drops is lost.
// Timing knobs: command and attribute latency, turnout travel, mast
// confirmation time; jammed points and stuck masts; physics hooks for
// trains and crews.
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/appliance/IControl.h"
#include "codeline/appliance/ISensor.h"
#include "codeline/appliance/ISignalMast.h"
#include "codeline/appliance/ITurnout.h"
#include "codeline/core/TickClock.h"

namespace codeline::testing {

enum class StandInHealth : uint8_t { Ok, Offline, Faulted, Unavailable };

// A remote appliance answers some scans later, never in the same one.
inline constexpr uint32_t kDefaultLatencyMs = 150;

// Built field by field so it survives AttributeInfo growing new fields.
inline const AttributeInfo* standInBoolAttr(const char* path) {
  static AttributeInfo offline{}, faulted{};
  AttributeInfo& a = path[12] == 'o' ? offline : faulted;  // "maintenance.offline" / ".faulted"
  a.path = path;
  a.type = AttrType::Bool;
  a.persistence = Persistence::Volatile;
  a.access = Access::RO;
  a.max_value = 1u;
  return &a;
}

// Cluster:MAINTENANCE as far as the engine uses it, plus fault injection.
// Derived classes supply emitState(), emitEvent() and dropInFlight(), and
// call tickMaintenance() from every tick() after power-on.
template <class Base>
class StandInMaintenance : public Base {
public:
  void identify(IdentifyMode, uint32_t) override {}
  // Asked now; answered from a later tick().
  void exportAttributes() override {
    ++attribute_requests_;
    if (!reachable()) return;
    if (!attr_pending_) attr_timer_ = 0;
    attr_pending_ = true;
  }
  void exportConfig() override {}
  void offline() override {}
  void online() override {}
  void setAttribute(std::string_view, SetOp, AttrValue) override {}
  void clearAttribute(std::string_view) override {}
  void clearFault() override {}
  void importConfig(const AttrMap&) override {}
  void cycle(uint16_t, uint32_t) override {}

  // Fault injection, with the events a core or a binding would emit.
  void setHealth(StandInHealth h) {
    const StandInHealth was = health_;
    if (h == was) return;
    health_ = h;
    if (!booted_) return;
    switch (h) {
      case StandInHealth::Unavailable:
        attr_pending_ = false;
        dropInFlight();
        emitEvent(ApplianceEvent::of(EventKind::Unavailable));
        break;
      case StandInHealth::Offline:
        if (was == StandInHealth::Unavailable) emitState();
        emitEvent(ApplianceEvent::of(EventKind::Offline));
        break;
      case StandInHealth::Faulted: {
        if (was == StandInHealth::Unavailable) emitState();
        ApplianceEvent ev = ApplianceEvent::of(EventKind::Fault);
        ev.fault = FaultType::HwError;
        emitEvent(ev);
        break;
      }
      case StandInHealth::Ok:
        emitState();  // reconnect replays retained state; a restart reports state
        if (was != StandInHealth::Unavailable) emitEvent(ApplianceEvent::of(EventKind::Online));
        break;
    }
  }
  StandInHealth health() const { return health_; }
  uint32_t attributeRequests() const { return attribute_requests_; }

  void setLatencyMs(uint32_t ms) { latency_ms_ = ms; }
  void setAttributeLatencyMs(uint32_t ms) { attr_latency_ms_ = ms; }

protected:
  virtual void emitState() = 0;
  virtual void emitEvent(const ApplianceEvent& ev) = 0;
  virtual void dropInFlight() = 0;

  bool reachable() const { return booted_ && health_ != StandInHealth::Unavailable; }

  // Answers a pending request for attributes once its latency has passed.
  void tickMaintenance() {
    if (!attr_pending_ || !reachable() || static_cast<uint32_t>(attr_timer_) < attr_latency_ms_) return;
    attr_pending_ = false;
    AttrMap m;
    m.add(standInBoolAttr("maintenance.offline"), AttrValue::boolean(health_ == StandInHealth::Offline));
    m.add(standInBoolAttr("maintenance.faulted"), AttrValue::boolean(health_ == StandInHealth::Faulted));
    ApplianceEvent ev = ApplianceEvent::of(EventKind::Attributes);
    ev.data = &m;
    emitEvent(ev);
  }

  TickClock clock_;
  bool booted_ = false;
  StandInHealth health_ = StandInHealth::Ok;
  uint32_t attribute_requests_ = 0;
  uint32_t latency_ms_ = kDefaultLatencyMs;

private:
  bool attr_pending_ = false;
  ElapsedMillis attr_timer_{clock_};
  uint32_t attr_latency_ms_ = 0;
};

inline constexpr uint32_t kManual = 0xFFFFFFFFu;  // turnout moves only by hand hooks

class StandInTurnout final : public StandInMaintenance<ITurnout> {
public:
  void setAlignment(TurnoutCommand c) override {
    if (!reachable() || c == TurnoutCommand::Unchanged) return;  // lost on a dead link
    pending_ = true;
    pending_cmd_ = c;
    pending_timer_ = 0;
    ++commands_;
    last_command_ = c;
  }
  TurnoutState state() const override { return state_; }
  void setObserver(ITurnoutObserver* o) override { observer_ = o; }
  void tick(uint32_t now_ms) override {
    clock_.advanceTo(now_ms);
    if (!booted_) {
      booted_ = true;
      state_ = points_;
      emitState();
      return;
    }
    tickMaintenance();
    if (hand_pending_) {
      hand_pending_ = false;
      setPoints(hand_);
    }
    if (pending_ && static_cast<uint32_t>(pending_timer_) >= latency_ms_) {
      pending_ = false;
      const TurnoutState target = pending_cmd_ == TurnoutCommand::Normal ? TurnoutState::Normal : TurnoutState::Reverse;
      if (target != points_) {
        target_ = target;
        moving_ = true;
        travel_timer_ = 0;
        setPoints(TurnoutState::InTransit);
      }
    }
    if (moving_ && !jammed_ && travel_ms_ != kManual && static_cast<uint32_t>(travel_timer_) >= travel_ms_) {
      moving_ = false;
      setPoints(target_);
    }
  }

  // Knobs and physics.
  void setTravelMs(uint32_t ms) { travel_ms_ = ms; }
  void setJammed(bool j) { jammed_ = j; }
  // A crew throws the points (or the contacts as found at power-on). After
  // power-on it takes effect, and is reported, at the next tick.
  void handThrow(TurnoutState s) {
    moving_ = false;
    pending_ = false;
    if (!booted_) {
      points_ = s;
      return;
    }
    hand_ = s;
    hand_pending_ = true;
  }
  uint32_t commands() const { return commands_; }
  TurnoutCommand lastCommand() const { return last_command_; }

private:
  void setPoints(TurnoutState s) {
    points_ = s;
    state_ = s;
    if (health_ != StandInHealth::Unavailable) emitState();
  }
  void emitState() override {
    if (observer_) observer_->onStateChanged(state_);
  }
  void emitEvent(const ApplianceEvent& ev) override {
    if (observer_) observer_->onEvent(ev);
  }
  void dropInFlight() override { pending_ = false; }

  ITurnoutObserver* observer_ = nullptr;
  TurnoutState points_ = TurnoutState::Normal;
  TurnoutState state_ = TurnoutState::InTransit;  // unproven before power-on
  TurnoutState target_ = TurnoutState::Normal;
  bool pending_ = false;
  TurnoutCommand pending_cmd_ = TurnoutCommand::Normal;
  ElapsedMillis pending_timer_{clock_};
  bool moving_ = false;
  ElapsedMillis travel_timer_{clock_};
  bool hand_pending_ = false;
  TurnoutState hand_ = TurnoutState::Normal;
  bool jammed_ = false;
  uint32_t travel_ms_ = 1000;
  uint32_t commands_ = 0;
  TurnoutCommand last_command_ = TurnoutCommand::Unchanged;
};

class StandInSensor final : public StandInMaintenance<ISensor> {
public:
  bool state() const override { return state_; }
  void setObserver(ISensorObserver* o) override { observer_ = o; }
  void tick(uint32_t now_ms) override {
    clock_.advanceTo(now_ms);
    if (!booted_) {
      booted_ = true;
      state_ = detect_;
      emitState();
      return;
    }
    tickMaintenance();
    if (state_ != detect_ && health_ != StandInHealth::Unavailable) {
      state_ = detect_;
      emitState();
    }
  }
  void shunt(bool occupied) { detect_ = occupied; }  // a train (or none); reported at the next tick

private:
  void emitState() override {
    if (observer_) observer_->onStateChanged(state_);
  }
  void emitEvent(const ApplianceEvent& ev) override {
    if (observer_) observer_->onEvent(ev);
  }
  void dropInFlight() override {}

  ISensorObserver* observer_ = nullptr;
  bool detect_ = false;
  bool state_ = true;  // unproven: occupied until the first report
};

class StandInMast final : public StandInMaintenance<IMast> {
public:
  void setIndication(SignalIndication i, bool lit) override {
    if (!reachable()) return;
    pending_ = true;
    pending_ind_ = i;
    pending_lit_ = lit;
    pending_timer_ = 0;
    ++commands_;
    last_command_ = i;
  }
  // Takes effect inside a later tick, like any command.
  void setLit(bool lit) override {
    if (!reachable()) return;
    lit_pending_ = true;
    lit_value_ = lit;
    lit_timer_ = 0;
  }
  MastState state() const override { return state_; }
  void setObserver(IMastObserver* o) override { observer_ = o; }
  void tick(uint32_t now_ms) override {
    clock_.advanceTo(now_ms);
    if (!booted_) {
      booted_ = true;
      emitState();
      return;
    }
    tickMaintenance();
    if (lit_pending_ && static_cast<uint32_t>(lit_timer_) >= latency_ms_) {
      lit_pending_ = false;
      setLitNow(lit_value_);
    }
    if (pending_ && static_cast<uint32_t>(pending_timer_) >= latency_ms_) {
      pending_ = false;
      setLitNow(pending_lit_);
      if (pending_ind_ != target_ || !settled_) {
        // The heads start to move. Cluster:MAST STATE: while they move the
        // mast holds the prior confirmed indication and emits nothing, for
        // every change (to STOP, to a proceed, proceed to proceed).
        target_ = pending_ind_;
        settled_ = false;
        confirm_timer_ = 0;
      }
    }
    if (!settled_ && !stuck_ && !failed_ && static_cast<uint32_t>(confirm_timer_) >= confirm_ms_) {
      settled_ = true;
      show(target_);
    }
  }

  void setConfirmMs(uint32_t ms) { confirm_ms_ = ms; }
  // The heads stop part-way: the mast keeps reporting the prior indication.
  void setStuck(bool s) { stuck_ = s; }
  // An unrecoverable head failure: the mast raises a FaultEvent (it never
  // reports STOP for it) and its heads stay as they are.
  void failHead() {
    failed_ = true;
    setHealth(StandInHealth::Faulted);
  }
  uint32_t commands() const { return commands_; }
  SignalIndication lastCommand() const { return last_command_; }
  SignalIndication displayed() const { return shown_; }  // what a crew sees, link or not

private:
  void setLitNow(bool lit) {
    if (state_.lit == lit) return;
    state_.lit = lit;
    if (health_ != StandInHealth::Unavailable) emitState();
  }
  void show(SignalIndication i) {
    shown_ = i;
    if (health_ == StandInHealth::Unavailable) return;  // nobody hears it
    if (state_.indication != i) {
      state_.indication = i;
      emitState();
    }
  }
  void emitState() override {
    state_.indication = shown_;
    if (observer_) observer_->onStateChanged(state_);
  }
  void emitEvent(const ApplianceEvent& ev) override {
    if (observer_) observer_->onEvent(ev);
  }
  void dropInFlight() override {
    pending_ = false;
    lit_pending_ = false;
  }

  IMastObserver* observer_ = nullptr;
  MastState state_;
  SignalIndication shown_ = SignalIndication::Stop;
  SignalIndication target_ = SignalIndication::Stop;
  bool settled_ = true;
  bool stuck_ = false;
  bool failed_ = false;
  bool pending_ = false;
  SignalIndication pending_ind_ = SignalIndication::Stop;
  bool pending_lit_ = true;
  ElapsedMillis pending_timer_{clock_};
  bool lit_pending_ = false;
  bool lit_value_ = true;
  ElapsedMillis lit_timer_{clock_};
  ElapsedMillis confirm_timer_{clock_};
  uint32_t confirm_ms_ = 0;
  uint32_t commands_ = 0;
  SignalIndication last_command_ = SignalIndication::Stop;
};

// The electric switch lock profile: SW1 lock open, BTN1 request, LAMP1 release.
class StandInLock final : public StandInMaintenance<IControl> {
public:
  void setLamps(BitVector lamps) override {
    if (!reachable() || lamps.count != 1) return;
    pending_ = true;
    pending_lamps_ = lamps;
    pending_timer_ = 0;
    ++commands_;
  }
  BitVector state() const override { return state_; }
  void setObserver(IControlObserver* o) override { observer_ = o; }
  void tick(uint32_t now_ms) override {
    clock_.advanceTo(now_ms);
    if (!booted_) {
      booted_ = true;
      state_ = contacts();
      emitState();
      return;
    }
    tickMaintenance();
    if (pending_ && static_cast<uint32_t>(pending_timer_) >= latency_ms_) {
      pending_ = false;
      lamps_ = pending_lamps_;
    }
    if (press_pending_ && reachable()) {
      press_pending_ = false;
      ApplianceEvent ev = ApplianceEvent::of(EventKind::Button);
      ev.channel = 1;
      ev.pressed = press_;
      emitEvent(ev);
    }
    const BitVector now = contacts();
    if (now != state_ && health_ != StandInHealth::Unavailable) {
      state_ = now;
      emitState();
    }
  }

  bool releaseLit() const { return lamps_.get(1); }
  // A crew opens or closes the lock; reported at the next tick. The lock
  // can be opened only while the release indicator is lit (the solenoid is
  // energized).
  bool open(bool o) {
    if (o && !releaseLit()) return false;
    open_ = o;
    return true;
  }
  // A crew presses or lets go of the request button; reported at the next tick.
  void pressRequest(bool pressed) {
    press_pending_ = true;
    press_ = pressed;
  }
  // A miswired or wrong-profile CONTROL: reports this many input channels.
  void setContactCount(uint8_t n) { contact_count_ = n; }
  uint32_t commands() const { return commands_; }

private:
  BitVector contacts() const {
    BitVector b;
    b.count = contact_count_;
    b.set(1, open_);
    return b;
  }
  void emitState() override {
    if (observer_) observer_->onStateChanged(state_);
  }
  void emitEvent(const ApplianceEvent& ev) override {
    if (observer_) observer_->onEvent(ev);
  }
  void dropInFlight() override {
    pending_ = false;
    press_pending_ = false;
  }

  IControlObserver* observer_ = nullptr;
  BitVector state_;  // count 0: unproven before power-on
  BitVector lamps_{0, 1};
  bool pending_ = false;
  BitVector pending_lamps_{0, 1};
  ElapsedMillis pending_timer_{clock_};
  bool press_pending_ = false;
  bool press_ = false;
  bool open_ = false;
  uint8_t contact_count_ = 1;
  uint32_t commands_ = 0;
};

}  // namespace codeline::testing
