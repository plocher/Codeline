// Appliance:CONTROL, C++ binding: the typed logical API on top (IControl),
// the observer it reports through, and the narrow driver port underneath
// (IControlDriver). Same shape as ITurnout.h. See docs/bindings/Cpp.md
// sections 2-3, docs/clusters/README.Clusters.md (LEVER, BUTTON) and
// docs/appliances/README.Appliances.md (Appliance:CONTROL).
//
// In M2 this is the electric switch lock lock:827 (docs/bindings/MQTT.md
// section 5a): SW1 asserted = lock open / HAND, (SW1) = locked normal;
// LAMP1 = release indicator; BTN1 (button.mode LEVEL) = crew unlock request.
//
// Unproven state (docs gap: BitVector has no "unproven" value). Choice:
// state() is a BitVector with count == 0 until the appliance has reported,
// and a proxy falls back to count == 0 whenever it loses the appliance. A
// frame whose count is not the lever.inputs the binding expects proves
// nothing; vital logic substitutes its fail-safe (for a lock: unlocked,
// docs/Codeline.md "Executive-Owned Vital Fail-Safe Substitution").
#pragma once

#include <cstdint>

#include "codeline/appliance/IMaintenance.h"

namespace codeline {

// Atomic all-or-nothing multi-channel frame (docs/bindings/Cpp.md section 2).
// Channel k (1-based, as SW1 / LAMP1 / BTN1) is bit k-1.
struct BitVector {
  static constexpr uint8_t kMaxCount = 32;

  uint32_t bits = 0;
  uint8_t count = 0;  // number of channels; 0 = no frame

  bool has(uint8_t channel) const { return channel >= 1 && channel <= count; }
  bool get(uint8_t channel) const { return has(channel) && ((bits >> (channel - 1)) & 1u) != 0; }
  void set(uint8_t channel, bool on) {
    if (!has(channel)) return;
    const uint32_t mask = 1u << (channel - 1);
    bits = on ? (bits | mask) : (bits & ~mask);
  }
  bool operator==(const BitVector& o) const { return bits == o.bits && count == o.count; }
  bool operator!=(const BitVector& o) const { return !(*this == o); }
};

// ButtonEvent (Cluster:BUTTON) arrives through onEvent as
// ApplianceEvent{kind = EventKind::Button, channel, pressed}.
struct IControlObserver {
  virtual ~IControlObserver() = default;
  virtual void onStateChanged(BitVector switches) = 0;
  virtual void onEvent(const ApplianceEvent& ev) = 0;
};

// Identical for the local core and for a remote appliance behind MQTT.
struct IControl : public IMaintenance {
  // Every feedback output 1..lever.outputs at once. If lamps.count !=
  // lever.outputs the whole command is rejected and no output changes.
  virtual void setLamps(BitVector lamps) = 0;
  // Last confirmed switch/lever contacts 1..lever.inputs; count == 0 until
  // first confirmation.
  virtual BitVector state() const = 0;
  virtual void setObserver(IControlObserver* o) = 0;
  virtual void tick(uint32_t now_ms) = 0;
};

struct IControlDriver {
  virtual ~IControlDriver() = default;
  // Raw contacts, channel k in bit k-1, already debounced and
  // polarity-corrected below the seam. The core keeps channels
  // 1..lever.inputs (resp. button.inputs) and ignores the rest.
  virtual uint32_t readSwitches() const = 0;
  virtual uint32_t readButtons() const = 0;
  // Outputs 1..lever.outputs, channel k in bit k-1.
  virtual void driveLamps(uint32_t bits) = 0;
  virtual void setIdentifyBeacon(bool on) = 0;
};

}  // namespace codeline
