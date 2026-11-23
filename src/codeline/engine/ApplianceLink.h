// The engine's observer on each bound appliance. It forwards every observer
// call to one ApplianceHealth (codeline/appliance/ApplianceHealth.h) and,
// for a CONTROL, remembers which buttons are held. State itself is read only
// by sampling state() once per scan, so a callback that arrives between
// scans changes nothing the current scan sees.
//
// Binding registers the link as the appliance's observer (the appliance
// has one observer slot, so a bound appliance cannot be observed by anyone
// else). A link made with bind false never registers: it belongs to an
// engine object the plant refused, which must not steal the slot. Bind before the appliance's first tick() (its power-on), or the
// engine never sees the first state report and keeps the appliance unproven.
#pragma once

#include <cstdint>

#include "codeline/appliance/ApplianceHealth.h"
#include "codeline/appliance/IControl.h"
#include "codeline/appliance/ISensor.h"
#include "codeline/appliance/ISignalMast.h"
#include "codeline/appliance/ITurnout.h"

namespace codeline::engine {

// Asks for the proof of health when it is due, then says whether the
// appliance is usable. Call once per scan, before reading state().
inline bool admit(ApplianceHealth& h, IMaintenance& appliance, uint32_t now_ms) {
  if (h.attributesDue(now_ms)) appliance.exportAttributes();
  return h.ok();
}

class SensorLink final : public ISensorObserver {
public:
  SensorLink(ISensor& s, bool bind) {
    if (bind) s.setObserver(this);
  }
  ApplianceHealth health;
  void onStateChanged(bool) override { health.onState(); }
  void onEvent(const ApplianceEvent& ev) override { health.onEvent(ev); }
};

class TurnoutLink final : public ITurnoutObserver {
public:
  TurnoutLink(ITurnout& t, bool bind) {
    if (bind) t.setObserver(this);
  }
  ApplianceHealth health;
  void onStateChanged(TurnoutState) override { health.onState(); }
  void onEvent(const ApplianceEvent& ev) override { health.onEvent(ev); }
};

class MastLink final : public IMastObserver {
public:
  MastLink(IMast& m, bool bind) {
    if (bind) m.setObserver(this);
  }
  ApplianceHealth health;
  uint32_t reports = 0;  // state reports received (SignalMast: proof of STOP needs one after its command)
  void onStateChanged(MastState) override {
    ++reports;
    health.onState();
  }
  void onEvent(const ApplianceEvent& ev) override { health.onEvent(ev); }
};

class ControlLink final : public IControlObserver {
public:
  ControlLink(IControl& c, bool bind) {
    if (bind) c.setObserver(this);
  }
  ApplianceHealth health;
  bool buttonHeld(uint8_t channel) const {
    return channel >= 1 && channel <= 32 && ((held_ >> (channel - 1)) & 1u) != 0;
  }
  void onStateChanged(BitVector) override { health.onState(); }
  void onEvent(const ApplianceEvent& ev) override {
    if (ev.kind == EventKind::Button) {
      if (ev.channel < 1 || ev.channel > 32) return;
      const uint32_t bit = 1u << (ev.channel - 1);
      held_ = ev.pressed ? (held_ | bit) : (held_ & ~bit);
      return;
    }
    health.onEvent(ev);
    if (ev.kind == EventKind::Unavailable || ev.kind == EventKind::Online) held_ = 0;
  }

private:
  uint32_t held_ = 0;
};

}  // namespace codeline::engine
