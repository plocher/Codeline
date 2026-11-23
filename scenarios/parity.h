// PARITY_SCENARIO(Traits, "name") { ... rig ... }: one scenario body, written
// against RigOf<Traits>, registered twice: direct (core over the simulated
// driver) and through the MQTT loopback (proxy -> broker -> adapter -> core).
#pragma once

#include "codeline/testing/ApplianceRig.h"
#include "scenario.h"

#define PARITY_SCENARIO(T, name)                                                                  \
  static void SCENARIO_CAT(parity_body_, __LINE__)(codeline::testing::RigOf<T> & rig);            \
  static codeline::scenario::Registrar SCENARIO_CAT(parity_direct_, __LINE__)(                    \
      "direct  " name, [] {                                                                      \
        codeline::testing::DirectRigOf<T> r;                                                     \
        SCENARIO_CAT(parity_body_, __LINE__)(r);                                                 \
      });                                                                                        \
  static codeline::scenario::Registrar SCENARIO_CAT(parity_mqtt_, __LINE__)(                      \
      "mqtt    " name, [] {                                                                      \
        codeline::testing::MqttRigOf<T> r;                                                       \
        SCENARIO_CAT(parity_body_, __LINE__)(r);                                                 \
      });                                                                                        \
  static void SCENARIO_CAT(parity_body_, __LINE__)(codeline::testing::RigOf<T> & rig)

namespace codeline::testing {

// The maintainer's sequence for a faulted appliance, as a controller sees it
// through ApplianceHealth: OFFLINE, repair (an OFFLINE write that also fixes
// NV), CLEAR FAULT, ONLINE. Health must read Faulted while faulted, never Ok
// before ONLINE, and Ok once the restart has been proven by attributes.
// The rig must already hold the bad NV value that faults the appliance.
template <class T, class Repair>
void maintainerClearsFault(RigOf<T>& rig, Repair repair) {
  HealthBinding<T> b;
  rig.app().setObserver(&b);
  uint32_t t = 0;
  for (; t <= 2000; t += 50) scan(rig, b, t);
  CHECK(b.health.health() == Health::Faulted);
  rig.app().offline();
  repair(rig.app());
  rig.app().clearFault();
  bool ok_too_soon = false;
  for (; t <= 6000; t += 50) {
    scan(rig, b, t);
    ok_too_soon |= b.health.ok();
  }
  CHECK(!ok_too_soon);
  rig.app().online();
  CHECK(!b.health.ok());
  for (; t <= 6000 + ApplianceHealth::kRetryMs + 100; t += 50) scan(rig, b, t);
  CHECK(b.health.ok());
}

}  // namespace codeline::testing
