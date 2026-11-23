// Appliance:TURNOUT, C++ binding: the typed logical API on top (ITurnout),
// the observer it reports through, and the narrow driver port underneath
// (ITurnoutDriver). See docs/bindings/Cpp.md section 3.
#pragma once

#include <cstdint>

#include "codeline/appliance/IMaintenance.h"
#include "codeline/generated/Enums.h"

namespace codeline {

struct ITurnoutObserver {
  virtual ~ITurnoutObserver() = default;
  virtual void onStateChanged(TurnoutState state) = 0;
  virtual void onEvent(const ApplianceEvent& ev) = 0;
};

// Identical for the local core and for a remote appliance behind MQTT.
struct ITurnout : public IMaintenance {
  virtual void setAlignment(TurnoutCommand cmd) = 0;
  virtual TurnoutState state() const = 0;  // last confirmed physical state
  virtual void setObserver(ITurnoutObserver* o) = 0;
  virtual void tick(uint32_t now_ms) = 0;
};

struct TurnoutContacts {
  bool normal;
  bool reverse;
};

struct ITurnoutDriver {
  virtual ~ITurnoutDriver() = default;
  virtual void driveMotor(TurnoutCommand dir) = 0;  // Unchanged = de-energize
  virtual TurnoutContacts readContacts() const = 0;
  virtual void setIdentifyBeacon(bool on) = 0;
};

}  // namespace codeline
