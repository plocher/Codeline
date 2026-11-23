// ApplianceHealth: how a controller learns that a bound appliance is usable
// (proven ONLINE, not faulted, reachable). Exercised on Appliance:TURNOUT,
// the one appliance with a core today, both direct and through the MQTT
// loopback. Also compiles the M2 interface headers (ISensor, IMast,
// IControl) under the project flags.
#include <type_traits>

#include "codeline/appliance/ApplianceHealth.h"
#include "codeline/appliance/IControl.h"
#include "codeline/appliance/ISensor.h"
#include "codeline/appliance/ISignalMast.h"
#include "codeline/generated/ControlAppliance.h"
#include "codeline/generated/SensorAppliance.h"
#include "codeline/generated/SignalmastAppliance.h"
#include "codeline/testing/TurnoutRig.h"
#include "scenario.h"

using namespace codeline;
using codeline::mqtt::InMemoryMockBroker;
using codeline::testing::DirectRig;
using codeline::testing::MqttRig;
using codeline::testing::TurnoutRig;

static_assert(std::is_abstract_v<ISensor> && std::is_abstract_v<ISensorDriver>);
static_assert(std::is_abstract_v<IMast> && std::is_abstract_v<IMastDriver>);
static_assert(std::is_abstract_v<IControl> && std::is_abstract_v<IControlDriver>);
static_assert(std::is_base_of_v<IMaintenance, ISensor> && std::is_base_of_v<IMaintenance, IMast> &&
              std::is_base_of_v<IMaintenance, IControl>);

// What a controller's per-appliance binding does: forward to the health
// tracker, and ask for attributes when it says so.
struct Binding final : ITurnoutObserver {
  ApplianceHealth health;
  void onStateChanged(TurnoutState) override { health.onState(); }
  void onEvent(const ApplianceEvent& ev) override { health.onEvent(ev); }
};

// One controller scan at now_ms: appliances first, then the binding's request.
static void scan(TurnoutRig& rig, Binding& b, uint32_t now_ms) {
  rig.tick(now_ms);
  if (b.health.attributesDue(now_ms)) rig.turnout().exportAttributes();
}

#define HEALTH_SCENARIO(name)                                                                   \
  static void SCENARIO_CAT(health_body_, __LINE__)(TurnoutRig & rig);                           \
  static codeline::scenario::Registrar SCENARIO_CAT(health_direct_, __LINE__)(                   \
      "direct  " name, [] {                                                                     \
        DirectRig r;                                                                            \
        SCENARIO_CAT(health_body_, __LINE__)(r);                                                \
      });                                                                                       \
  static codeline::scenario::Registrar SCENARIO_CAT(health_mqtt_, __LINE__)(                     \
      "mqtt    " name, [] {                                                                     \
        MqttRig r;                                                                              \
        SCENARIO_CAT(health_body_, __LINE__)(r);                                                \
      });                                                                                       \
  static void SCENARIO_CAT(health_body_, __LINE__)(TurnoutRig & rig)

HEALTH_SCENARIO("unproven until attributes prove it; asks only after state, and retries") {
  Binding b;
  rig.turnout().setObserver(&b);
  CHECK(b.health.health() == Health::Unproven);
  CHECK(!b.health.attributesDue(0));  // nothing reported yet: the appliance may not be up
  rig.tick(0);
  CHECK(b.health.health() == Health::Unproven);  // state alone proves nothing
  CHECK(b.health.attributesDue(0));
  CHECK(!b.health.attributesDue(ApplianceHealth::kRetryMs - 1));  // request lost: wait
  CHECK(b.health.attributesDue(ApplianceHealth::kRetryMs));       // then ask again
  rig.turnout().exportAttributes();
  CHECK(b.health.health() == Health::Ok);
  CHECK(!b.health.attributesDue(ApplianceHealth::kRetryMs * 5));
}

HEALTH_SCENARIO("OFFLINE is unhealthy; ONLINE is unproven until re-read") {
  Binding b;
  rig.turnout().setObserver(&b);
  scan(rig, b, 0);
  CHECK(b.health.ok());
  rig.turnout().offline();
  CHECK(b.health.health() == Health::Offline);
  rig.turnout().online();
  CHECK(b.health.health() == Health::Unproven);
  scan(rig, b, 10);
  CHECK(b.health.ok());
}

HEALTH_SCENARIO("a fault stays unhealthy until a restart proves it gone") {
  Binding b;
  rig.turnout().setObserver(&b);
  scan(rig, b, 0);
  rig.driver().shortContacts();
  scan(rig, b, 10);
  CHECK(b.health.health() == Health::Faulted);
  rig.driver().releaseContacts();
  scan(rig, b, 20);
  CHECK(b.health.health() == Health::Faulted);  // latched while ONLINE

  // Still shorted at the restart: the restart re-raises it before ONLINE.
  rig.driver().shortContacts();
  rig.turnout().offline();
  rig.turnout().online();
  scan(rig, b, 30);
  CHECK(b.health.health() == Health::Faulted);

  // Repaired, cleared OFFLINE (clearFault emits nothing), restarted.
  rig.driver().releaseContacts();
  rig.turnout().offline();
  rig.turnout().clearFault();
  CHECK(b.health.health() == Health::Offline);
  rig.turnout().online();
  CHECK(b.health.health() == Health::Unproven);
  scan(rig, b, 40);
  CHECK(b.health.ok());
}

HEALTH_SCENARIO("a NON_VITAL fault does not make the appliance unhealthy") {
  Binding b;
  rig.turnout().setObserver(&b);
  scan(rig, b, 0);
  ApplianceEvent ev = ApplianceEvent::of(EventKind::Fault);
  ev.fault = FaultType::NonVitalError;
  b.onEvent(ev);
  CHECK(b.health.ok());
  ev.fault = FaultType::HwError;
  b.onEvent(ev);
  CHECK(b.health.health() == Health::Faulted);
}

SCENARIO("mqtt    losing the appliance is UNAVAILABLE; it is re-proven after it returns") {
  MqttRig rig;
  Binding b;
  rig.turnout().setObserver(&b);
  scan(rig, b, 0);
  CHECK(b.health.ok());
  rig.broker().dropConnection(rig.applianceClient());
  CHECK(b.health.health() == Health::Unavailable);
  CHECK(!b.health.attributesDue(10));  // nobody to ask
  rig.adapter().connect();
  CHECK(b.health.health() == Health::Unproven);
  scan(rig, b, 20);
  CHECK(b.health.ok());
}

SCENARIO("mqtt    an OFFLINE missed while the controller's link was down is found on return") {
  MqttRig rig;
  Binding b;
  rig.turnout().setObserver(&b);
  scan(rig, b, 0);
  CHECK(b.health.ok());
  rig.broker().dropConnection(rig.officeClient());
  CHECK(b.health.health() == Health::Unavailable);

  // A maintainer takes the appliance OFFLINE; its /event OFFLINE is not retained.
  InMemoryMockBroker::Client maintainer(rig.broker());
  maintainer.connect("maintainer", nullptr);
  maintainer.publish(rig.topic("maint"), "OFFLINE", false);

  rig.proxy().connect("office");  // the retained /state alone would look fine
  CHECK(rig.turnout().state() == TurnoutState::Normal);
  CHECK(b.health.health() == Health::Unproven);
  scan(rig, b, 10);
  CHECK(b.health.health() == Health::Offline);
}

SCENARIO("BitVector channels are 1-based and bounded by count") {
  BitVector v;
  CHECK(v.count == 0 && !v.get(1));  // unproven: no channels at all
  v.count = 2;
  v.set(1, true);
  v.set(3, true);  // outside count: ignored
  CHECK(v.get(1) && !v.get(2) && !v.get(3));
  CHECK(v.bits == 1u);
  CHECK(MastState{} == (MastState{SignalIndication::Stop, true}));
}

int main() { return codeline::scenario::run(); }
