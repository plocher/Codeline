// Cluster:MAINTENANCE for a Reference Appliance Core, written once: gates,
// attributes and NV, identify, faults, and the RESTART BEHAVIOR sequence of
// docs/clusters/README.Clusters.md. SensorCore, MastCore, ControlCore and LampCore
// derive from it and supply the domain half through the hooks below.
// (TurnoutCore predates this base and still carries its own copy.)
//
//   Interface  the typed API the core implements (ISensor, IMast, IControl)
//   Observer   its observer type
//   Schema     the generated <Name>Schema traits: attribute table, faults
//
// Time advances only through tick(now_ms). The first tick() is power-on: it
// runs RESTART BEHAVIOR, so set the observer before it. Every command that
// arrives before power-on is dropped, and nothing reaches the driver.
//
// POST (restart step 4): maintenance.post defaults true on these appliances,
// but no non-moving self-test is defined for them, and exercising a signal
// head or a lock-release output at boot would show the layout something no
// controller asked for. So POST runs nothing; the restart's hardware
// re-verification (step 3) is the whole check.
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/appliance/IMaintenance.h"
#include "codeline/appliance/INvStore.h"
#include "codeline/core/Attributes.h"
#include "codeline/core/TickClock.h"
#include "codeline/generated/Enums.h"

namespace codeline {

template <class Interface, class Observer, class Schema>
class MaintenanceCore : public Interface {
public:
  void setObserver(Observer* o) override { observer_ = o; }

  void tick(uint32_t now_ms) override {
    clock_.advanceTo(now_ms);
    if (!booted_) {
      booted_ = true;
      restart(false);
      return;
    }
    updateIdentify();
    tickDomain();
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
    values_[kOffline] = 1;
    emit(ApplianceEvent::of(EventKind::Offline));
  }

  void online() override {
    if (accepts(MaintenanceCommand::Online)) restart(isOffline());
  }

  void setAttribute(std::string_view path, SetOp op, AttrValue value) override {
    if (!accepts(MaintenanceCommand::SetAttribute)) return;
    const int i = Schema::table.indexOf(path);
    if (i < 0 || !writable(Schema::attributes[i])) return;
    uint32_t next = 0;
    if (!applyOp(Schema::attributes[i], values_[i], op, value.raw, next)) return;
    store(i, next);
    announce(Schema::attributes[i]);
  }

  void clearAttribute(std::string_view path) override {
    if (!accepts(MaintenanceCommand::ClearAttribute)) return;
    const int i = Schema::table.indexOf(path);
    if (i < 0) return;
    const AttributeInfo& a = Schema::attributes[i];
    // RO-but-clearable counters (fault_count) are gated like VITAL.
    const bool gate_open = a.access == Access::RW || isOffline();
    if (!a.clearable || !gate_open) return;
    store(i, a.default_value);
    announce(a);
  }

  // Re-verifies both what can raise a fault here, the hardware condition and
  // the stored NV values, and clears maintenance.faulted only if both pass.
  void clearFault() override {
    if (!accepts(MaintenanceCommand::ClearFault)) return;
    if (!faultConditionPresent() && nvValid()) values_[kFaulted] = 0;
  }

  // All-or-nothing: one entry that is unknown, not NV config, or out of range
  // rejects the whole import: nothing is written and no ConfigEvent is emitted.
  void importConfig(const AttrMap& config) override {
    if (!accepts(MaintenanceCommand::ImportConfig)) return;
    if (config.count == 0 || !importable(Schema::table, config)) return;  // empty: nothing to import
    for (uint8_t e = 0; e < config.count; ++e) {
      store(Schema::table.indexOf(config.entries[e].info->path), config.entries[e].value.raw);
    }
    emitMap(EventKind::Config);
  }

  void cycle(uint16_t count, uint32_t dwell_ms) override {
    if (!accepts(MaintenanceCommand::Cycle) || count == 0) return;
    startCycle(count, dwell_ms);
  }

protected:
  using Attr = typename Schema::Attr;

  explicit MaintenanceCore(INvStore& nv) : nv_(nv) {
    for (uint8_t i = 0; i < Schema::attr_count; ++i) values_[i] = Schema::attributes[i].default_value;
  }

  // ---- domain hooks ----

  // Restart step 1: cancel any cycle or motion; leave every output in a
  // deterministic state.
  virtual void stopActivity() = 0;
  // Restart step 3: re-verify the hardware; raise a fault for what fails.
  virtual void verifyHardware() {}
  // For clearFault(): is a fault condition still physically present?
  virtual bool faultConditionPresent() const { return false; }
  // Restart step 5: enter the Startup State and report it (always, even if
  // unchanged). A core whose state must be proven first (MastCore) reports it
  // once proven, after CONFIG and ONLINE.
  virtual void enterStartup() = 0;
  // Every tick after power-on.
  virtual void tickDomain() = 0;
  // cycle(), already gated OFFLINE and count > 0.
  virtual void startCycle(uint16_t count, uint32_t dwell_ms) = 0;
  // An attribute value changed (set, clear, import, NV reload not included).
  virtual void onStored(int /*index*/) {}
  virtual void driveBeacon(bool on) = 0;

  // ---- for the domain half ----

  TickClock clock_;
  Observer* observer_ = nullptr;

  bool booted() const { return booted_; }
  bool isOffline() const { return values_[kOffline] != 0; }
  static constexpr int idx(Attr a) { return static_cast<int>(a); }
  uint32_t get(Attr a) const { return values_[idx(a)]; }

  // A volatile attribute that is the domain half's own state seen as an
  // attribute (onoff.on): the domain half keeps it current here. No event, no
  // NV write and no onStored().
  void mirror(Attr a, uint32_t v) { values_[idx(a)] = v; }

  // Only faults the schema lets this appliance raise compile.
  template <FaultType T>
  void raiseFault(const char* detail) {
    static_assert(Schema::raises(T), "schema/appliances/ does not list this fault for this appliance");
    const bool was_faulted = values_[kFaulted] != 0 || refaulting_;
    if (T != FaultType::NonVitalError) values_[kFaulted] = 1;
    if (T == FaultType::NonVitalError || !was_faulted) {
      const uint32_t n = values_[kFaultCount];
      if (n != UINT32_MAX) store(kFaultCount, n + 1);
    }
    ApplianceEvent ev = ApplianceEvent::of(EventKind::Fault);
    ev.fault = T;
    ev.detail = detail;
    emit(ev);
  }

  void emit(const ApplianceEvent& ev) {
    if (observer_) observer_->onEvent(ev);
  }

private:
  static constexpr int kOffline = static_cast<int>(Attr::MaintenanceOffline);
  static constexpr int kId = static_cast<int>(Attr::MaintenanceId);
  static constexpr int kFaulted = static_cast<int>(Attr::MaintenanceFaulted);
  static constexpr int kFaultCount = static_cast<int>(Attr::MaintenanceFaultCount);

  INvStore& nv_;
  uint32_t values_[Schema::attr_count];
  bool booted_ = false;
  bool refaulting_ = false;  // restart re-verifying a fault it already had
  bool pulse_ = false;
  uint32_t pulse_ms_ = 0;
  ElapsedMillis identify_timer_{clock_};  // read only while pulse_

  bool accepts(MaintenanceCommand c) const {
    if (!booted_) return false;
    return kMaintenanceCommandGates[static_cast<uint8_t>(c)] != CommandGate::Offline || isOffline();
  }

  bool writable(const AttributeInfo& a) const {
    return a.access == Access::RW || (a.access == Access::Vital && isOffline());
  }

  static bool applyOp(const AttributeInfo& a, uint32_t current, SetOp op, uint32_t operand, uint32_t& out) {
    int64_t next = operand;
    if (op != SetOp::Assign) {
      if (a.type != AttrType::Uint && a.type != AttrType::Hundredths) return false;  // only numbers can be nudged
      next = op == SetOp::Increment ? int64_t(current) + operand : int64_t(current) - operand;
    }
    if (next < int64_t(a.min_value) || next > int64_t(a.max_value)) return false;
    out = static_cast<uint32_t>(next);
    return true;
  }

  void store(int i, uint32_t v) {
    values_[i] = v;
    if (Schema::attributes[i].persistence != Persistence::Volatile) nv_.save(Schema::attributes[i].path, v);
    onStored(i);
  }

  void announce(const AttributeInfo& a) {
    emitMap(a.persistence == Persistence::NvConfig ? EventKind::Config : EventKind::Attributes);
  }

  // Returns false if any stored value was out of range (defaults used instead).
  bool loadNv() {
    bool ok = true;
    for (uint8_t i = 0; i < Schema::attr_count; ++i) {
      const AttributeInfo& a = Schema::attributes[i];
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

  // Whether every stored NV value is in range now (absent counts as valid).
  bool nvValid() const {
    for (uint8_t i = 0; i < Schema::attr_count; ++i) {
      const AttributeInfo& a = Schema::attributes[i];
      uint32_t v = 0;
      if (a.persistence != Persistence::Volatile && nv_.load(a.path, v) && !isValid(a, AttrValue{v})) return false;
    }
    return true;
  }

  // ---- RESTART BEHAVIOR (power-on and online()) ----

  void restart(bool exiting_offline) {
    // 1. Cancel cycles and motion; outputs to a deterministic state.
    stopActivity();
    // 2. Reload NV config.
    const bool config_ok = loadNv();
    // 3. Volatile attributes back to their declared defaults; re-verify.
    //    A fault found again here was already counted: a restart is not a
    //    transition from confirmed healthy.
    refaulting_ = values_[kFaulted] != 0;
    for (uint8_t i = 0; i < Schema::attr_count; ++i) {
      if (Schema::attributes[i].persistence == Persistence::Volatile) values_[i] = Schema::attributes[i].default_value;
    }
    pulse_ = false;
    driveBeacon(values_[kId] != 0);
    if (!config_ok) raiseFault<FaultType::ConfigError>("NV config out of range; defaults used");
    verifyHardware();
    refaulting_ = false;
    // 4. POST: none (see the file comment).
    // 5. Startup State, then CONFIG, then ONLINE last.
    enterStartup();
    emitMap(EventKind::Config);
    if (exiting_offline) emit(ApplianceEvent::of(EventKind::Online));
  }

  void setBeacon(bool on) {
    values_[kId] = on ? 1 : 0;
    driveBeacon(on);
  }

  void updateIdentify() {
    if (pulse_ && identify_timer_ >= pulse_ms_) {
      pulse_ = false;
      setBeacon(false);
    }
  }

  // Attributes: volatile and NV state. Config: NV config.
  void emitMap(EventKind kind) {
    AttrMap map;
    for (uint8_t i = 0; i < Schema::attr_count; ++i) {
      const bool config = Schema::attributes[i].persistence == Persistence::NvConfig;
      if (config == (kind == EventKind::Config)) map.add(&Schema::attributes[i], AttrValue{values_[i]});
    }
    ApplianceEvent ev = ApplianceEvent::of(kind);
    ev.data = &map;
    emit(ev);
  }
};

}  // namespace codeline
