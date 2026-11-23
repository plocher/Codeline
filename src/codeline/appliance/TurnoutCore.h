// Reference Appliance Core for Appliance:TURNOUT: Cluster:MAINTENANCE plus
// Cluster:TURNOUT over any ITurnoutDriver. Optional POSITION and GROUPS are
// not composed in this slice.
//
// Sources: docs/clusters/README.Clusters.md (MAINTENANCE, TURNOUT),
// docs/appliances/README.Appliances.md (Appliance:TURNOUT),
// docs/bindings/Cpp.md section 3. Gates, attributes, defaults and the faults
// this appliance may raise come from schema/ via codeline/generated/.
//
// Time advances only through tick(now_ms). The first tick() is power-on: it
// runs the RESTART BEHAVIOR sequence, so set the observer before it. Every
// command that arrives before power-on is dropped (nothing reaches the
// driver), and state() is IN_TRANSIT (unproven) until then.
//
// The core does not time a throw and raises no STALL: how long a motor runs,
// whether it stays energized at the end (a Tortoise does) and what its travel
// limits are belong to the driver. Points that never arrive simply stay
// IN_TRANSIT, which the logic above treats as out of correspondence. The core
// tells the driver to stop driving only on a restart.
//
// Timers are read only while the thing they time is active, so a turnout
// idle for 2^32 ms does not see a timer wrap back into range.
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/appliance/INvStore.h"
#include "codeline/appliance/ITurnout.h"
#include "codeline/core/TickClock.h"
#include "codeline/generated/TurnoutAppliance.h"

namespace codeline {

class TurnoutCore final : public ITurnout {
public:
  TurnoutCore(ITurnoutDriver& driver, INvStore& nv) : driver_(driver), nv_(nv) {
    for (uint8_t i = 0; i < kTurnoutAttrCount; ++i) values_[i] = kTurnoutAttributes[i].default_value;
  }

  // ---- Cluster:TURNOUT ---------------------------------------------------

  // Operational: accepted ONLINE and OFFLINE. Repeating the command the
  // points are already moving toward changes nothing (a code line re-sends
  // its controls), so it cannot restart emulated transit.
  void setAlignment(TurnoutCommand cmd) override {
    if (!booted_ || cmd == TurnoutCommand::Unchanged) return;
    if (moving_ && cmd == target_) return;
    cycle_.throws_left = 0;  // an operational command ends a bench cycle
    command(cmd);
  }

  TurnoutState state() const override { return alignment_; }
  void setObserver(ITurnoutObserver* o) override { observer_ = o; }

  void tick(uint32_t now_ms) override {
    clock_.advanceTo(now_ms);
    if (!booted_) {
      booted_ = true;
      restart(false);
      return;
    }
    updateIdentify();
    updateAlignment();
    updateCycle();
  }

  // ---- Cluster:MAINTENANCE -----------------------------------------------

  void identify(IdentifyMode mode, uint32_t duration_ms) override {
    if (!accepts(MaintenanceCommand::Identify)) return;
    pulse_ = mode == IdentifyMode::Pulse;
    pulse_ms_ = duration_ms;
    identify_timer_ = 0;
    setBeacon(mode != IdentifyMode::Off);
  }

  void exportAttributes() override {
    if (accepts(MaintenanceCommand::ExportAttributes)) emitMap(EventKind::Attributes);
  }
  void exportConfig() override {
    if (accepts(MaintenanceCommand::ExportConfig)) emitMap(EventKind::Config);
  }

  void offline() override {
    if (!accepts(MaintenanceCommand::Offline) || isOffline()) return;
    set(TurnoutAttr::MaintenanceOffline, 1);
    emit(ApplianceEvent::of(EventKind::Offline));
  }

  void online() override {
    if (accepts(MaintenanceCommand::Online)) restart(isOffline());
  }

  void setAttribute(std::string_view path, SetOp op, AttrValue value) override {
    if (!accepts(MaintenanceCommand::SetAttribute)) return;
    const int i = kTurnoutAttributeTable.indexOf(path);
    if (i < 0 || !writable(kTurnoutAttributes[i])) return;
    uint32_t next = 0;
    if (!applyOp(kTurnoutAttributes[i], values_[i], op, value.raw, next)) return;
    store(i, next);
    announce(kTurnoutAttributes[i]);
  }

  void clearAttribute(std::string_view path) override {
    if (!accepts(MaintenanceCommand::ClearAttribute)) return;
    const int i = kTurnoutAttributeTable.indexOf(path);
    if (i < 0) return;
    const AttributeInfo& a = kTurnoutAttributes[i];
    // RO-but-clearable counters (fault_count) are gated like VITAL.
    const bool gate_open = a.access == Access::RW || isOffline();
    if (!a.clearable || !gate_open) return;
    store(i, a.default_value);
    announce(a);
  }

  // Clears maintenance.faulted only if the fault condition is resolved.
  void clearFault() override {
    if (!accepts(MaintenanceCommand::ClearFault)) return;
    if (!contactsShorted(driver_.readContacts())) set(TurnoutAttr::MaintenanceFaulted, 0);
  }

  // All-or-nothing: one entry that is unknown, not NV config, or out of range
  // rejects the whole import: nothing is written and no ConfigEvent is emitted.
  void importConfig(const AttrMap& config) override {
    if (!accepts(MaintenanceCommand::ImportConfig)) return;
    if (config.count == 0 || !importable(kTurnoutAttributeTable, config)) return;  // empty: nothing to import
    for (uint8_t e = 0; e < config.count; ++e) {
      store(kTurnoutAttributeTable.indexOf(config.entries[e].info->path), config.entries[e].value.raw);
    }
    emitMap(EventKind::Config);
  }

  // One cycle is a throw to the far end and back, dwelling at each end.
  void cycle(uint16_t count, uint32_t dwell_ms) override {
    if (!accepts(MaintenanceCommand::Cycle) || count == 0) return;
    cycle_.throws_left = 2u * count;
    cycle_.dwell_ms = dwell_ms;
    cycle_.dwelling = false;
    command(alignment_ == TurnoutState::Reverse ? TurnoutCommand::Normal : TurnoutCommand::Reverse);
  }

private:
  struct Cycle {
    uint32_t throws_left = 0;
    uint32_t dwell_ms = 0;
    bool dwelling = false;
  };

  ITurnoutDriver& driver_;
  INvStore& nv_;
  ITurnoutObserver* observer_ = nullptr;

  TickClock clock_;
  ElapsedMillis emulated_timer_{clock_};  // read only while emulating_
  ElapsedMillis identify_timer_{clock_};  // read only while pulse_
  ElapsedMillis dwell_timer_{clock_};     // read only while cycle_.dwelling

  uint32_t values_[kTurnoutAttrCount];
  bool booted_ = false;
  TurnoutState alignment_ = TurnoutState::InTransit;
  TurnoutCommand target_ = TurnoutCommand::Unchanged;  // last command
  bool moving_ = false;     // target not reached yet
  bool motor_on_ = false;   // we energized the motor and have not released it
  TurnoutCommand emulated_ = TurnoutCommand::Unchanged;  // Unchanged = nothing emulated
  bool emulating_ = false;  // emulated transit still running
  bool shorted_ = false;
  bool refaulting_ = false;  // restart re-verifying a fault it already had
  bool pulse_ = false;
  uint32_t pulse_ms_ = 0;
  Cycle cycle_;

  // ---- gates and attributes ----

  bool accepts(MaintenanceCommand c) const {
    if (!booted_) return false;
    return kMaintenanceCommandGates[static_cast<uint8_t>(c)] != CommandGate::Offline || isOffline();
  }

  static constexpr uint8_t idx(TurnoutAttr a) { return static_cast<uint8_t>(a); }
  uint32_t get(TurnoutAttr a) const { return values_[idx(a)]; }
  void set(TurnoutAttr a, uint32_t v) { values_[idx(a)] = v; }
  bool isOffline() const { return get(TurnoutAttr::MaintenanceOffline) != 0; }
  FeedbackRegime regime() const { return static_cast<FeedbackRegime>(get(TurnoutAttr::TurnoutFeedbackRegime)); }
  uint32_t simTransitMs() const { return get(TurnoutAttr::TurnoutSimTransitMs); }

  bool writable(const AttributeInfo& a) const {
    return a.access == Access::RW || (a.access == Access::Vital && isOffline());
  }

  static bool applyOp(const AttributeInfo& a, uint32_t current, SetOp op, uint32_t operand, uint32_t& out) {
    int64_t next = operand;
    if (op != SetOp::Assign) {
      if (a.type != AttrType::Uint) return false;  // only numbers can be nudged
      next = op == SetOp::Increment ? int64_t(current) + operand : int64_t(current) - operand;
    }
    if (next < int64_t(a.min_value) || next > int64_t(a.max_value)) return false;
    out = static_cast<uint32_t>(next);
    return true;
  }

  void store(int i, uint32_t v) {
    values_[i] = v;
    if (kTurnoutAttributes[i].persistence != Persistence::Volatile) nv_.save(kTurnoutAttributes[i].path, v);
    // What the old regime emulated means nothing under the new one.
    if (i == idx(TurnoutAttr::TurnoutFeedbackRegime)) forgetEmulation();
  }

  void announce(const AttributeInfo& a) {
    emitMap(a.persistence == Persistence::NvConfig ? EventKind::Config : EventKind::Attributes);
  }

  // Returns false if any stored value was out of range (defaults used instead).
  bool loadNv() {
    bool ok = true;
    for (uint8_t i = 0; i < kTurnoutAttrCount; ++i) {
      const AttributeInfo& a = kTurnoutAttributes[i];
      if (a.persistence == Persistence::Volatile) continue;
      uint32_t v = 0;
      if (!nv_.load(a.path, v)) {
        values_[i] = a.default_value;
      } else if (!isValid(a, AttrValue{v})) {
        values_[i] = a.default_value;
        ok = false;
      } else {
        values_[i] = v;
      }
    }
    return ok;
  }

  // ---- RESTART BEHAVIOR (power-on and online()) ----

  void restart(bool exiting_offline) {
    // 1. Cancel cycle and motion; leave the motor de-energized.
    cycle_.throws_left = 0;
    moving_ = false;
    forgetEmulation();
    releaseMotor();
    // 2. Reload NV config.
    const bool config_ok = loadNv();
    // 3. Volatile attributes back to their declared defaults; re-verify.
    //    A fault found again here was already counted: a restart is not a
    //    transition from confirmed healthy.
    refaulting_ = get(TurnoutAttr::MaintenanceFaulted) != 0;
    for (uint8_t i = 0; i < kTurnoutAttrCount; ++i) {
      if (kTurnoutAttributes[i].persistence == Persistence::Volatile) values_[i] = kTurnoutAttributes[i].default_value;
    }
    pulse_ = false;
    driver_.setIdentifyBeacon(get(TurnoutAttr::MaintenanceId) != 0);
    if (!config_ok) raiseFault<FaultType::ConfigError>("NV config out of range; defaults used");
    const TurnoutContacts c = driver_.readContacts();
    shorted_ = contactsShorted(c);
    if (shorted_) raiseFault<FaultType::ShortedContacts>("both point contacts closed");
    refaulting_ = false;
    // 4. POST: a TURNOUT never moves on its own (maintenance.post defaults
    //    false), and this slice defines no non-moving self-test, so none runs.
    // 5. Startup State: report the contacts as read under the current regime.
    alignment_ = evaluate(c);
    if (observer_) observer_->onStateChanged(alignment_);
    emitMap(EventKind::Config);
    if (exiting_offline) emit(ApplianceEvent::of(EventKind::Online));
  }

  // ---- alignment ----

  static TurnoutState toState(TurnoutCommand c) {
    return c == TurnoutCommand::Normal ? TurnoutState::Normal : TurnoutState::Reverse;
  }

  void command(TurnoutCommand cmd) {
    driver_.driveMotor(cmd);
    motor_on_ = true;
    target_ = cmd;
    moving_ = true;
    // Emulated contacts take sim_transit_ms only if the points must move.
    emulated_ = cmd;
    emulating_ = alignment_ != toState(cmd);
    emulated_timer_ = 0;
  }

  void releaseMotor() {
    if (!motor_on_) return;
    driver_.driveMotor(TurnoutCommand::Unchanged);
    motor_on_ = false;
  }

  void forgetEmulation() {
    emulated_ = TurnoutCommand::Unchanged;
    emulating_ = false;
  }

  bool contactsShorted(TurnoutContacts c) const {
    return regime() == FeedbackRegime::TwoContact && c.normal && c.reverse;
  }

  TurnoutState evaluate(TurnoutContacts c) const {
    const bool emulated_done = emulated_ != TurnoutCommand::Unchanged && !emulating_;
    switch (regime()) {
      case FeedbackRegime::TwoContact:
        if (c.normal == c.reverse) return TurnoutState::InTransit;  // moving, or shorted
        return c.normal ? TurnoutState::Normal : TurnoutState::Reverse;
      case FeedbackRegime::OneContact:
        if (c.normal) return TurnoutState::Normal;
        return emulated_ == TurnoutCommand::Reverse && emulated_done ? TurnoutState::Reverse
                                                                     : TurnoutState::InTransit;
      case FeedbackRegime::Simulated:
        return emulated_done ? toState(emulated_) : TurnoutState::InTransit;
    }
    return TurnoutState::InTransit;
  }

  void updateAlignment() {
    if (emulating_ && emulated_timer_ >= simTransitMs()) emulating_ = false;
    const TurnoutContacts c = driver_.readContacts();
    const TurnoutState now = evaluate(c);
    if (now != alignment_) {
      alignment_ = now;
      if (observer_) observer_->onStateChanged(alignment_);
    }
    const bool shorted = contactsShorted(c);
    if (shorted && !shorted_) raiseFault<FaultType::ShortedContacts>("both point contacts closed");
    shorted_ = shorted;

    if (moving_ && alignment_ == toState(target_)) moving_ = false;
  }

  void updateCycle() {
    if (cycle_.throws_left == 0 || moving_) return;
    if (!cycle_.dwelling) {
      cycle_.dwelling = true;
      dwell_timer_ = 0;
    } else if (dwell_timer_ >= cycle_.dwell_ms) {
      cycle_.dwelling = false;
      if (--cycle_.throws_left > 0) {
        command(target_ == TurnoutCommand::Normal ? TurnoutCommand::Reverse : TurnoutCommand::Normal);
      }
    }
  }

  // ---- identify ----

  void setBeacon(bool on) {
    set(TurnoutAttr::MaintenanceId, on ? 1 : 0);
    driver_.setIdentifyBeacon(on);
  }

  void updateIdentify() {
    if (pulse_ && identify_timer_ >= pulse_ms_) {
      pulse_ = false;
      setBeacon(false);
    }
  }

  // ---- events ----

  // Only faults the schema lets Appliance:TURNOUT raise compile.
  template <FaultType T>
  void raiseFault(const char* detail) {
    static_assert(turnoutRaises(T), "schema/appliances/turnout.json does not list this fault");
    const bool was_faulted = get(TurnoutAttr::MaintenanceFaulted) != 0 || refaulting_;
    if (T != FaultType::NonVitalError) set(TurnoutAttr::MaintenanceFaulted, 1);
    if (T == FaultType::NonVitalError || !was_faulted) {
      const uint32_t n = get(TurnoutAttr::MaintenanceFaultCount);
      if (n != UINT32_MAX) store(idx(TurnoutAttr::MaintenanceFaultCount), n + 1);
    }
    ApplianceEvent ev = ApplianceEvent::of(EventKind::Fault);
    ev.fault = T;
    ev.detail = detail;
    emit(ev);
  }

  // Attributes: volatile and NV state. Config: NV config.
  void emitMap(EventKind kind) {
    AttrMap map;
    for (uint8_t i = 0; i < kTurnoutAttrCount; ++i) {
      const bool config = kTurnoutAttributes[i].persistence == Persistence::NvConfig;
      if (config == (kind == EventKind::Config)) map.add(&kTurnoutAttributes[i], AttrValue{values_[i]});
    }
    ApplianceEvent ev = ApplianceEvent::of(kind);
    ev.data = &map;
    emit(ev);
  }

  void emit(const ApplianceEvent& ev) {
    if (observer_) observer_->onEvent(ev);
  }
};

}  // namespace codeline
