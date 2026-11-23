// A track circuit as vital logic sees it: one Appliance:SENSOR, sampled once
// per scan, with executive-owned fail-safe substitution.
//
// Harvested from FieldUnit src/TrackCircuit.h. Left out on purpose:
// - dropoutDelay: debouncing and gap bridging are Cluster:HOLDOFF, below the
//   seam (docs/clusters/README.Clusters.md, "Hardware Hygiene Below the Seam");
// - staleness timeouts: link loss now arrives as UNAVAILABLE from the binding.
// Spike section 6, item 9 and section 4, change 3.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "codeline/appliance/ISensor.h"
#include "codeline/engine/ApplianceLink.h"

namespace codeline::engine {

class TrackCircuit {
public:
  // `bind` false: a detached circuit that never observes the sensor (a
  // configuration the plant refused).
  TrackCircuit(std::string_view name, ISensor& sensor, bool bind = true)
      : name_(name), sensor_(sensor), link_(sensor, bind) {}

  TrackCircuit(const TrackCircuit&) = delete;
  TrackCircuit& operator=(const TrackCircuit&) = delete;

  const std::string& name() const { return name_; }

  // TR: proven vacant. Unhealthy, or not yet reported, reads occupied
  // (sample() substitutes active).
  bool clear() const { return !active_; }
  // Proven occupied: healthy and reporting a train. An unhealthy circuit
  // reads occupied for locking, but it is never evidence that a train
  // arrived (it does not accept a signal or start a sectional release).
  bool occupied() const { return healthy_ && active_; }
  bool healthy() const { return healthy_; }

  // Ingress, once per scan.
  void sample(uint32_t now_ms) {
    healthy_ = admit(link_.health, sensor_, now_ms);
    active_ = healthy_ ? sensor_.state() : true;
  }

private:
  std::string name_;
  ISensor& sensor_;
  SensorLink link_;
  bool healthy_ = false;
  bool active_ = true;
};

}  // namespace codeline::engine
