// Reference Appliance Core for Appliance:LAMP: Cluster:MAINTENANCE plus
// Cluster:ON/OFF over any ILampDriver.
//
// Sources: docs/clusters/README.Clusters.md (ON/OFF),
// docs/appliances/README.Appliances.md (Appliance:LAMP).
//
// setOn(on) drives the output and reports `on` when it changed. It is
// operational: accepted ONLINE and OFFLINE. onoff.on is the same output as
// an attribute (volatile, RW in both modes on a LAMP): writing it is setOn(),
// and it always reads what the output is.
//
// Startup State, and every restart: on = false, reported (always, even if it
// was already false). So a lamp that restarts goes dark until its controller
// states it again.
//
// cycle(count, dwell_ms) (OFFLINE): `count` passes of on for dwell_ms, then
// off for dwell_ms, each step reported as `on`; then the last commanded value
// is restored. A setOn() (or a write of onoff.on) ends a cycle.
//
// The driver port has no fault channel (the docs name no lamp fault), so the
// only fault this core raises is CONFIG_ERROR.
#pragma once

#include <cstdint>

#include "codeline/appliance/ILamp.h"
#include "codeline/appliance/MaintenanceCore.h"
#include "codeline/generated/LampAppliance.h"

namespace codeline {

class LampCore final : public MaintenanceCore<ILamp, ILampObserver, LampSchema> {
public:
  LampCore(ILampDriver& driver, INvStore& nv) : MaintenanceCore(nv), driver_(driver) {}

  // ---- Cluster:ON/OFF ----------------------------------------------------

  void setOn(bool on) override {
    if (!booted()) return;
    cycle_.active = false;
    commanded_ = on;
    show(on);
  }

  bool state() const override { return on_; }

private:
  struct Cycle {
    bool active = false;
    uint16_t passes_left = 0;
    uint32_t dwell_ms = 0;
  };

  ILampDriver& driver_;
  bool on_ = false;         // the output now
  bool commanded_ = false;  // the last setOn(); what a cycle restores
  ElapsedMillis dwell_timer_{clock_};  // read only while cycle_.active
  Cycle cycle_;

  // Drives the output; reports it if it changed.
  void show(bool on) {
    const bool changed = on != on_;
    on_ = on;
    mirror(Attr::OnoffOn, on ? 1 : 0);
    driver_.driveLamp(on);
    if (changed && observer_) observer_->onStateChanged(on_);
  }

  void updateCycle() {
    if (!cycle_.active || dwell_timer_ < cycle_.dwell_ms) return;
    dwell_timer_ = 0;
    if (on_) {
      show(false);
    } else if (--cycle_.passes_left > 0) {
      show(true);
    } else {
      cycle_.active = false;
      show(commanded_);
    }
  }

  // ---- MaintenanceCore hooks ----

  void startCycle(uint16_t count, uint32_t dwell_ms) override {
    cycle_ = Cycle{true, count, dwell_ms};
    dwell_timer_ = 0;
    show(true);
  }

  void stopActivity() override { cycle_.active = false; }

  void enterStartup() override {
    commanded_ = false;
    on_ = false;
    mirror(Attr::OnoffOn, 0);
    driver_.driveLamp(false);
    if (observer_) observer_->onStateChanged(on_);
  }

  void tickDomain() override { updateCycle(); }

  void onStored(int index) override {
    if (index == idx(Attr::OnoffOn)) setOn(get(Attr::OnoffOn) != 0);
  }

  void driveBeacon(bool on) override { driver_.setIdentifyBeacon(on); }
};

}  // namespace codeline
