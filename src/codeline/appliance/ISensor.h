// Appliance:SENSOR, C++ binding: the typed logical API on top (ISensor), the
// observer it reports through, and the narrow driver port underneath
// (ISensorDriver). Same shape as ITurnout.h. See docs/bindings/Cpp.md
// section 3, docs/clusters/README.Clusters.md (SENSOR, HOLDOFF) and
// docs/appliances/README.Appliances.md (Appliance:SENSOR).
//
// Unproven state (docs gap: Cluster:SENSOR's `active` has no "unproven"
// value). Choice, the most restrictive reading: state() is true (ACTIVE,
// occupied) until the appliance has reported, and a proxy falls back to true
// whenever it loses the appliance. A true from state() therefore does not
// distinguish a train from "nothing proven"; vital logic that needs to know
// which reads ApplianceHealth (codeline/appliance/ApplianceHealth.h).
#pragma once

#include <cstdint>

#include "codeline/appliance/IMaintenance.h"

namespace codeline {

struct ISensorObserver {
  virtual ~ISensorObserver() = default;
  virtual void onStateChanged(bool active) = 0;
  virtual void onEvent(const ApplianceEvent& ev) = 0;
};

// Identical for the local core and for a remote appliance behind MQTT.
// Cluster:SENSOR is read-only: there are no operational commands.
struct ISensor : public IMaintenance {
  // Last confirmed filtered detection (after HOLDOFF): true = Occupied /
  // Triggered, false = Vacant / Clear. True until first confirmation.
  virtual bool state() const = 0;
  virtual void setObserver(ISensorObserver* o) = 0;
  virtual void tick(uint32_t now_ms) = 0;
};

struct ISensorDriver {
  virtual ~ISensorDriver() = default;
  // Raw detection, already debounced and polarity-corrected by the hardware
  // or driver (Clusters.md "Hardware Hygiene Below the Seam"). HOLDOFF's
  // make/dropout timing is the core's, not the driver's.
  virtual bool readDetector() const = 0;
  virtual void setIdentifyBeacon(bool on) = 0;
};

}  // namespace codeline
