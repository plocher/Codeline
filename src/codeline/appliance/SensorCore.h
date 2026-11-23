// Reference Appliance Core for Appliance:SENSOR: Cluster:MAINTENANCE plus
// Cluster:SENSOR and Cluster:HOLDOFF over any ISensorDriver.
//
// Sources: docs/clusters/README.Clusters.md (SENSOR, HOLDOFF),
// docs/appliances/README.Appliances.md (Appliance:SENSOR).
//
// HOLDOFF: the raw detection must hold continuously for holdoff.make_ms
// before `active` turns true, and stay clear continuously for
// holdoff.dropout_ms before it turns false. A change of the raw input back to
// what is reported restarts the count. The input is sampled once per tick.
//
// Startup State (docs: "evaluates physical detection under active holdoff.*
// parameters"): with no history, the restrictive reading is that the circuit
// was occupied a moment ago. So startup begins at active = true and applies
// HOLDOFF from there: with dropout_ms = 0 a clear detector reports false at
// once, otherwise after dropout_ms of continuous clear.
//
// No commands, nothing to drive: cycle() is a no-op (accepted OFFLINE only),
// and the driver port has no fault channel, so the only fault this core
// raises is CONFIG_ERROR.
#pragma once

#include <cstdint>

#include "codeline/appliance/ISensor.h"
#include "codeline/appliance/MaintenanceCore.h"
#include "codeline/generated/SensorAppliance.h"

namespace codeline {

class SensorCore final : public MaintenanceCore<ISensor, ISensorObserver, SensorSchema> {
public:
  SensorCore(ISensorDriver& driver, INvStore& nv) : MaintenanceCore(nv), driver_(driver) {}

  bool state() const override { return active_; }

private:
  ISensorDriver& driver_;
  bool active_ = true;  // unproven: occupied
  bool timing_ = false;
  ElapsedMillis timer_{clock_};  // read only while timing_

  void stopActivity() override { timing_ = false; }

  void enterStartup() override {
    active_ = true;
    timing_ = false;
    evaluate();
    if (observer_) observer_->onStateChanged(active_);
  }

  void tickDomain() override {
    const bool before = active_;
    evaluate();
    if (active_ != before && observer_) observer_->onStateChanged(active_);
  }

  void startCycle(uint16_t, uint32_t) override {}  // read-only input appliance
  void driveBeacon(bool on) override { driver_.setIdentifyBeacon(on); }

  void evaluate() {
    const bool raw = driver_.readDetector();
    if (raw == active_) {
      timing_ = false;
      return;
    }
    if (!timing_) {
      timing_ = true;
      timer_ = 0;
    }
    const uint32_t hold = active_ ? get(Attr::HoldoffDropoutMs) : get(Attr::HoldoffMakeMs);
    if (timer_ >= hold) {
      active_ = raw;
      timing_ = false;
    }
  }
};

}  // namespace codeline
