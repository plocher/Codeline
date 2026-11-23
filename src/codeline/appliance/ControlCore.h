// Reference Appliance Core for Appliance:CONTROL: Cluster:MAINTENANCE plus
// Cluster:LEVER and Cluster:BUTTON over any IControlDriver.
//
// Sources: docs/clusters/README.Clusters.md (LEVER, BUTTON),
// docs/appliances/README.Appliances.md (Appliance:CONTROL), and the lock:827
// profile in docs/bindings/MQTT.md section 5a.
//
// switches: the raw contacts 1..lever.inputs, reported as one frame on boot
// and whenever any contact changes (sampled once per tick). lever.input_style
// is carried as configuration only: every style reports its contacts as read
// (no latching of momentary inputs; the docs define none).
//
// Buttons 1..button.inputs, sampled once per tick:
//   LEVEL            press -> ButtonEvent(k, true), release -> ButtonEvent(k, false)
//   ONESHOT_RELEASE  release -> ButtonEvent(k, true), once
// A restart takes the buttons as they are then without an event, so a
// button held across a restart is reported only when it next changes. A
// controller should forget held buttons on OnlineEvent and UnavailableEvent.
//
// setLamps(lamps) drives every output at once; a frame whose count is not
// lever.outputs is rejected whole. Startup and every restart extinguish the
// lamps. Changing lever.outputs (OFFLINE) extinguishes them too.
//
// cycle(count, dwell): lights lamp.1 .. lamp.M one at a time for dwell_ms
// each, for `count` passes, then restores the last setLamps() frame. An
// operational setLamps() ends a cycle.
//
// Channel counts above the BitVector width cannot be stored: the generator
// bounds lever.inputs, lever.outputs and button.inputs by
// schema/types.json composites.BitVector.width (kBitVectorWidth).
#pragma once

#include <cstdint>

#include "codeline/appliance/IControl.h"
#include "codeline/appliance/MaintenanceCore.h"
#include "codeline/generated/ControlAppliance.h"

namespace codeline {

static_assert(BitVector::kMaxCount == kBitVectorWidth, "IControl.h BitVector must match schema/types.json");

class ControlCore final : public MaintenanceCore<IControl, IControlObserver, ControlSchema> {
public:
  ControlCore(IControlDriver& driver, INvStore& nv) : MaintenanceCore(nv), driver_(driver) {}

  // ---- Cluster:LEVER -----------------------------------------------------

  // Operational: accepted ONLINE and OFFLINE.
  void setLamps(BitVector lamps) override {
    if (!booted() || lamps.count != outputs()) return;
    cycle_.active = false;
    lamps_ = BitVector{lamps.bits & mask(outputs()), lamps.count};
    driver_.driveLamps(lamps_.bits);
  }

  BitVector state() const override { return switches_; }

private:
  struct Cycle {
    bool active = false;
    uint16_t passes_left = 0;
    uint8_t lamp = 0;  // 1..outputs
    uint32_t dwell_ms = 0;
  };

  IControlDriver& driver_;
  BitVector switches_;  // count 0 until power-on
  BitVector lamps_;     // last setLamps() frame
  uint32_t buttons_ = 0;
  ElapsedMillis dwell_timer_{clock_};  // read only while cycle_.active
  Cycle cycle_;

  static uint32_t mask(uint32_t n) { return n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1u; }
  uint8_t inputs() const { return static_cast<uint8_t>(get(Attr::LeverInputs)); }
  uint8_t outputs() const { return static_cast<uint8_t>(get(Attr::LeverOutputs)); }
  uint32_t readButtons() const { return driver_.readButtons() & mask(get(Attr::ButtonInputs)); }

  BitVector readSwitches() const { return BitVector{driver_.readSwitches() & mask(inputs()), inputs()}; }

  void extinguish() {
    lamps_ = BitVector{0, outputs()};
    driver_.driveLamps(0);
  }

  void updateButtons() {
    const uint32_t now = readButtons();
    const uint32_t changed = now ^ buttons_;
    buttons_ = now;
    if (!changed) return;
    const bool level = static_cast<ButtonMode>(get(Attr::ButtonMode)) == ButtonMode::Level;
    for (uint8_t k = 1; k <= BitVector::kMaxCount; ++k) {
      const uint32_t bit = 1u << (k - 1);
      if (!(changed & bit)) continue;
      const bool pressed = (now & bit) != 0;
      if (!level && pressed) continue;  // ONESHOT_RELEASE reports the release only
      ApplianceEvent ev = ApplianceEvent::of(EventKind::Button);
      ev.channel = k;
      ev.pressed = level ? pressed : true;
      emit(ev);
    }
  }

  void updateCycle() {
    if (!cycle_.active || dwell_timer_ < cycle_.dwell_ms) return;
    if (cycle_.lamp < outputs()) {
      ++cycle_.lamp;
    } else if (--cycle_.passes_left > 0) {
      cycle_.lamp = 1;
    } else {
      cycle_.active = false;
      driver_.driveLamps(lamps_.bits);
      return;
    }
    dwell_timer_ = 0;
    driver_.driveLamps(1u << (cycle_.lamp - 1));
  }

  // ---- MaintenanceCore hooks ----

  void startCycle(uint16_t count, uint32_t dwell_ms) override {
    if (outputs() == 0) return;
    cycle_ = Cycle{true, count, 1, dwell_ms};
    dwell_timer_ = 0;
    driver_.driveLamps(1u);
  }

  void stopActivity() override {
    cycle_.active = false;
    extinguish();
  }

  void enterStartup() override {
    extinguish();
    buttons_ = readButtons();
    switches_ = readSwitches();
    if (observer_) observer_->onStateChanged(switches_);
  }

  void tickDomain() override {
    const BitVector now = readSwitches();
    if (now != switches_) {
      switches_ = now;
      if (observer_) observer_->onStateChanged(switches_);
    }
    updateButtons();
    updateCycle();
  }

  void onStored(int index) override {
    if (index == idx(Attr::LeverOutputs)) {
      cycle_.active = false;
      extinguish();
    } else if (index == idx(Attr::ButtonInputs)) {
      buttons_ = readButtons();  // newly counted channels start from what they read
    }
  }

  void driveBeacon(bool on) override { driver_.setIdentifyBeacon(on); }
};

}  // namespace codeline
