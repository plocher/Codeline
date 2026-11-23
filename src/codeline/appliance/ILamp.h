// Appliance:LAMP, C++ binding: the typed logical API on top (ILamp), the
// observer it reports through, and the narrow driver port underneath
// (ILampDriver). Same shape as ISensor.h and IControl.h. See
// docs/bindings/Cpp.md section 3, docs/clusters/README.Clusters.md (ON/OFF)
// and docs/appliances/README.Appliances.md (Appliance:LAMP).
//
// This slice is the binary lamp (a control point's maintainer call lamp):
// Cluster:MAINTENANCE and Cluster:ON/OFF. DIMMING, COLOR and GROUPS are not
// composed.
//
// Unproven state (docs gap: `on` has no "unproven" value). Choice: state() is
// false (not lit) until the appliance has reported, and a proxy falls back to
// false whenever it loses the appliance: nobody is told a lamp is lit that
// cannot be seen to be. A false from state() therefore does not distinguish
// a dark lamp from "nothing proven"; a controller that needs to know which
// reads ApplianceHealth (codeline/appliance/ApplianceHealth.h).
#pragma once

#include <cstdint>

#include "codeline/appliance/IMaintenance.h"

namespace codeline {

struct ILampObserver {
  virtual ~ILampObserver() = default;
  virtual void onStateChanged(bool on) = 0;
  virtual void onEvent(const ApplianceEvent& ev) = 0;
};

// Identical for the local core and for a remote appliance behind MQTT.
struct ILamp : public IMaintenance {
  // Cluster:ON/OFF, operational: the binary output.
  virtual void setOn(bool on) = 0;
  // Last confirmed output; false until first confirmation.
  virtual bool state() const = 0;
  virtual void setObserver(ILampObserver* o) = 0;
  virtual void tick(uint32_t now_ms) = 0;
};

struct ILampDriver {
  virtual ~ILampDriver() = default;
  // The binary output: a lamp, a horn, a relay.
  virtual void driveLamp(bool on) = 0;
  virtual void setIdentifyBeacon(bool on) = 0;
};

}  // namespace codeline
