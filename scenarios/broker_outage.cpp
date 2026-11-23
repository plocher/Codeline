// Broker outages, for all four MQTT appliances (TURNOUT, SENSOR, SIGNALMAST,
// CONTROL). Owner ruling: an appliance that loses its broker does nothing to
// its outputs (no watchdog; outputs stay as last driven; inputs keep being
// read). When the connection returns it must put the wire back in order:
// re-register its will, re-subscribe, republish its current retained /state,
// and never act on anything retained or sent before. A controller's proxy
// goes to its fallback + Unavailable on its own link loss, and after
// reconnecting is not Ok until a fresh /state and a fresh attributes proof.
//
// Cases, each run on each appliance:
//   A  the appliance's link drops and returns; the will left UNAVAILABLE
//   B  the same, and the broker lost its retained store meanwhile
//   C  the controller's link drops and returns
//   D  the broker restarts without persistence (both links drop at once)
//   E  the broker restarts with persistence (old retained states survive)
// Reconnection is the adapter's and proxy's own job, driven by their tick()
// on the ReconnectBackoff schedule (mqtt.json "reconnect").
#include <cstdio>
#include <string>
#include <vector>

#include "codeline/testing/ApplianceRig.h"
#include "codeline/testing/TurnoutRig.h"
#include "scenario.h"

using namespace codeline;
using namespace codeline::testing;
using codeline::mqtt::InMemoryMockBroker;

enum class Case { A, B, C, D, E };

// ---- one wrapper per appliance: the same questions, asked of each rig ----

template <class Observer, class State>
struct Binding final : Observer {
  ApplianceHealth health;
  bool returned = false;      // the outage is over
  bool proven_after = false;  // an AttributesEvent arrived after it
  void onStateChanged(State) override { health.onState(); }
  void onEvent(const ApplianceEvent& ev) override {
    if (returned && ev.kind == EventKind::Attributes) proven_after = true;
    health.onEvent(ev);
  }
};

struct TurnoutCase {
  using B = Binding<ITurnoutObserver, TurnoutState>;
  MqttRig rig;
  IMaintenance& app() { return rig.turnout(); }
  void observe(B* b) { rig.turnout().setObserver(b); }
  void prime() { rig.turnout().setAlignment(TurnoutCommand::Reverse); }
  uint32_t outputs() { return rig.driver().driveCount() * 4 + static_cast<uint32_t>(rig.driver().motor()); }
  void commandDuringOutage() { rig.turnout().setAlignment(TurnoutCommand::Normal); }
  void inputDuringOutage() {}
  const char* staleCmd() { return "NORMAL (REVERSE)"; }
  std::string coreText() { return mqtt::encodeState(rig.core().state()); }
  std::string proxyText() { return mqtt::encodeState(rig.turnout().state()); }
  const char* fallback() { return "(NORMAL) (REVERSE)"; }
};

template <class T>
struct ApplianceCase {
  using B = Binding<typename T::Observer, typename T::State>;
  MqttRigOf<T> rig;
  IMaintenance& app() { return rig.app(); }
  void observe(B* b) { rig.app().setObserver(b); }
  std::string coreText() { return T::text(rig.core().state()); }
  std::string proxyText() { return T::text(rig.app().state()); }
};

struct SensorCase : ApplianceCase<SensorTraits> {
  void prime() {}
  uint32_t outputs() { return 0; }  // no outputs
  void commandDuringOutage() {}
  void inputDuringOutage() { rig.driver().shunt(true); }  // a train arrives meanwhile
  const char* staleCmd() { return "(ACTIVE)"; }
  const char* fallback() { return "ACTIVE"; }
};

struct MastCase : ApplianceCase<MastTraits> {
  void prime() { rig.app().setIndication(SignalIndication::Clear, true); }
  uint32_t outputs() { return rig.driver().setCount(0) * 16 + static_cast<uint32_t>(rig.driver().commanded(0)); }
  void commandDuringOutage() { rig.app().setIndication(SignalIndication::Stop, true); }
  void inputDuringOutage() {}
  const char* staleCmd() { return "STOP"; }
  const char* fallback() { return "STOP"; }
};

struct ControlCase : ApplianceCase<ControlTraits> {
  ControlCase() { rig.nv().save("lever.outputs", 1); }
  void prime() { rig.app().setLamps(BitVector{1, 1}); }
  uint32_t outputs() { return rig.driver().driveCount() * 2 + rig.driver().lamps(); }
  void commandDuringOutage() { rig.app().setLamps(BitVector{0, 1}); }
  void inputDuringOutage() { rig.driver().setSwitch(1, true); }  // the maintainer turns the lock lever
  const char* staleCmd() { return "(LAMP1)"; }
  const char* fallback() { return "-"; }
};

template <class W>
static void outage(Case c) {
  W w;
  typename W::B b;
  w.observe(&b);
  uint32_t t = 0;
  auto scan = [&](uint32_t until) {
    for (; t <= until; t += 50) {
      w.rig.tick(t);
      if (b.health.attributesDue(t)) w.app().exportAttributes();
    }
  };
  scan(2000);
  w.prime();
  scan(4000);
  CHECK(b.health.ok());

  // Before the outage a rogue leaves a command retained; it must never act.
  InMemoryMockBroker::Client rogue(w.rig.broker());
  rogue.connect("rogue", nullptr);
  rogue.publish(w.rig.topic("cmd"), w.staleCmd(), true);
  rogue.disconnect();
  const uint32_t out0 = w.outputs();

  InMemoryMockBroker& broker = w.rig.broker();
  switch (c) {
    case Case::A:
    case Case::B:
      broker.setReachable(w.rig.applianceClient(), false);
      broker.dropConnection(w.rig.applianceClient());
      CHECK(*w.rig.broker().retained(w.rig.topic("state")) == "UNAVAILABLE");
      if (c == Case::B) broker.clearRetained();
      break;
    case Case::C:
      broker.setReachable(w.rig.officeClient(), false);
      broker.dropConnection(w.rig.officeClient());
      break;
    case Case::D:
    case Case::E:
      broker.goDown(c == Case::E);
      break;
  }
  CHECK(!b.health.ok());
  CHECK(w.proxyText() == w.fallback());

  // During the outage: the office still commands, inputs still change.
  w.commandDuringOutage();
  w.inputDuringOutage();
  bool ok_during = false;
  for (const uint32_t until = t + 3000; t <= until; t += 50) {
    w.rig.tick(t);
    if (b.health.attributesDue(t)) w.app().exportAttributes();
    ok_during |= b.health.ok();
  }
  CHECK(!ok_during);
  CHECK(w.outputs() == out0);  // nothing done to the outputs

  // The connection returns.
  broker.setReachable(w.rig.applianceClient(), true);
  broker.setReachable(w.rig.officeClient(), true);
  broker.comeUp();
  b.returned = true;
  bool ok_unproven = false;
  for (const uint32_t until = t + 25000; t <= until; t += 50) {  // backoff: the next retry may be up to 20 s away
    w.rig.tick(t);
    if (b.health.attributesDue(t)) w.app().exportAttributes();
    ok_unproven |= b.health.ok() && !b.proven_after;
  }
  CHECK(!ok_unproven);  // never Ok on a stale proof
  CHECK(b.health.ok());
  const std::string* retained = broker.retained(w.rig.topic("state"));
  CHECK(retained && *retained == w.coreText());  // republished, retained, current
  CHECK(w.proxyText() == w.coreText());
  CHECK(w.outputs() == out0);  // no command from before or during the outage acted
}

// First connect: until the core has reported a state, /state reads the will
// payload, so a stale retained state from an earlier session never reads as
// this boot's state.
template <class W>
static void firstConnect() {
  W w;
  const std::string* r = w.rig.broker().retained(w.rig.topic("state"));
  CHECK(r && *r == "UNAVAILABLE");
  for (uint32_t t = 0; t <= 2000; t += 50) w.rig.tick(t);
  r = w.rig.broker().retained(w.rig.topic("state"));
  CHECK(r && *r == w.coreText());
}
SCENARIO("turnout first connect: /state is UNAVAILABLE until the core reports") { firstConnect<TurnoutCase>(); }
SCENARIO("sensor  first connect: /state is UNAVAILABLE until the core reports") { firstConnect<SensorCase>(); }
SCENARIO("mast    first connect: /state is UNAVAILABLE until the core reports") { firstConnect<MastCase>(); }
SCENARIO("control first connect: /state is UNAVAILABLE until the core reports") { firstConnect<ControlCase>(); }

// ---- the retry schedule itself ----

// The times (on a 10 ms tick grid) at which ReconnectBackoff, keyed by `key`,
// would try connect() after a loss noticed at `origin`, up to `horizon`.
static std::vector<uint32_t> schedule(std::string_view key, uint32_t origin, uint32_t horizon) {
  const uint32_t skew = mqtt::ReconnectBackoff::skewPermille(key);
  std::vector<uint32_t> at;
  uint32_t base = mqtt::kReconnectInitialMs, from = origin;
  for (;;) {
    const uint32_t wait = base + base * skew / 1000u;
    const uint32_t due = from + (wait + 9) / 10 * 10;
    if (due - origin > horizon) return at;
    at.push_back(due);
    from = due;
    base = base * mqtt::kReconnectFactor > mqtt::kReconnectMaxMs ? mqtt::kReconnectMaxMs : base * mqtt::kReconnectFactor;
  }
}

// Ticks a rig every 10 ms from `from` to `to` and returns the times at which
// `client` tried connect().
template <class Rig>
static std::vector<uint32_t> attempts(Rig& rig, InMemoryMockBroker::Client& client, uint32_t from, uint32_t to) {
  std::vector<uint32_t> at;
  uint32_t n = client.connectAttempts();
  for (uint32_t t = from;; t += 10) {
    rig.tick(t);
    if (client.connectAttempts() != n) {
      n = client.connectAttempts();
      at.push_back(t);
    }
    if (t == to) return at;
  }
}

SCENARIO("backoff a 60 s appliance outage: a handful of attempts, each at its scheduled time") {
  MqttRigOf<MastTraits> rig;
  rig.tick(0);
  rig.tick(1000);
  rig.broker().setReachable(rig.applianceClient(), false);
  rig.broker().dropConnection(rig.applianceClient());  // noticed at t = 1000
  const std::vector<uint32_t> got = attempts(rig, rig.applianceClient(), 1010, 61000);
  const std::vector<uint32_t> want = schedule(rig.adapter().applianceTopic(), 1000, 60000);
  CHECK(got == want);
  CHECK(got.size() >= 5 && got.size() <= 8);  // 1, 2, 4, 8, 16, 16 ... s (+ skew)
  CHECK(got.size() >= 2 && got[1] - got[0] > mqtt::kReconnectInitialMs);
  for (size_t i = 1; i + 1 < got.size(); ++i) CHECK(got[i + 1] - got[i] >= got[i] - got[i - 1]);
  CHECK(got.back() - got[got.size() - 2] >= mqtt::kReconnectMaxMs);  // settled at the cap
  CHECK(got.back() - got[got.size() - 2] <= mqtt::kReconnectMaxMs * (1000 + mqtt::kReconnectSkewPermille) / 1000 + 10);
}

SCENARIO("backoff a 60 s controller outage: the proxy's attempts follow the same schedule") {
  MqttRigOf<MastTraits> rig;
  rig.tick(0);
  rig.tick(1000);
  rig.broker().setReachable(rig.officeClient(), false);
  rig.broker().dropConnection(rig.officeClient());
  const std::vector<uint32_t> got = attempts(rig, rig.officeClient(), 1010, 61000);
  CHECK(got == schedule("office", 1000, 60000));
  CHECK(got.size() >= 5 && got.size() <= 8);
}

SCENARIO("backoff a successful reconnect resets the sequence") {
  MqttRigOf<SensorTraits> rig;
  rig.tick(0);
  rig.tick(1000);
  rig.broker().setReachable(rig.applianceClient(), false);
  rig.broker().dropConnection(rig.applianceClient());
  const std::vector<uint32_t> first = attempts(rig, rig.applianceClient(), 1010, 30000);  // backed off past 8 s
  rig.broker().setReachable(rig.applianceClient(), true);
  const std::vector<uint32_t> back = attempts(rig, rig.applianceClient(), 30010, 60000);
  CHECK(back.size() == 1);  // reconnected at the next scheduled attempt, then no more
  CHECK(rig.applianceClient().connected());
  rig.broker().setReachable(rig.applianceClient(), false);
  rig.broker().dropConnection(rig.applianceClient());  // noticed at t = 60000
  const std::vector<uint32_t> again = attempts(rig, rig.applianceClient(), 60010, 64000);
  CHECK(!again.empty() && again == schedule(rig.adapter().applianceTopic(), 60000, 4000));
  CHECK(again[0] - 60000 < 2 * mqtt::kReconnectInitialMs);  // from 1 s again, not from 16 s
  CHECK(first.size() >= 4);
}

SCENARIO("backoff two appliances that lose the broker together retry at different times") {
  MqttRigOf<MastTraits> mast;
  MqttRigOf<ControlTraits> lock;
  const uint32_t skew_mast = mqtt::ReconnectBackoff::skewPermille(mast.adapter().applianceTopic());
  const uint32_t skew_lock = mqtt::ReconnectBackoff::skewPermille(lock.adapter().applianceTopic());
  CHECK(skew_mast != skew_lock);
  mast.tick(1000);
  lock.tick(1000);
  mast.broker().goDown(true);  // the same instant for both
  lock.broker().goDown(true);
  const std::vector<uint32_t> a = attempts(mast, mast.applianceClient(), 1010, 40000);
  const std::vector<uint32_t> b = attempts(lock, lock.applianceClient(), 1010, 40000);
  CHECK(a.size() >= 3 && b.size() >= 3);
  bool all_differ = true;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) all_differ &= a[i] != b[i];
  CHECK(all_differ);
  std::printf("        830WAB skew %u/1000, first retries at +%u +%u +%u ms; lock:827 skew %u/1000, at +%u +%u +%u ms\n",
              skew_mast, a[0] - 1000, a[1] - 1000, a[2] - 1000, skew_lock, b[0] - 1000, b[1] - 1000, b[2] - 1000);
}

SCENARIO("backoff a long idle across the clock wrap gives one attempt, not a burst") {
  MqttRigOf<SensorTraits> rig;
  const uint32_t t0 = 0xFFFFF000u;
  rig.tick(t0);
  rig.broker().setReachable(rig.applianceClient(), false);
  rig.broker().dropConnection(rig.applianceClient());
  const uint32_t n = rig.applianceClient().connectAttempts();
  rig.tick(t0 + 0x40000000u);  // a long idle that wraps past zero
  CHECK(rig.applianceClient().connectAttempts() == n + 1);
  rig.tick(t0 + 0x40000000u + 10);
  CHECK(rig.applianceClient().connectAttempts() == n + 1);
  const std::vector<uint32_t> next = attempts(rig, rig.applianceClient(), t0 + 0x40000000u + 20, t0 + 0x40000000u + 4000);
  CHECK(next.size() == 1);  // the second wait (about 2 s), counted from the attempt
  CHECK(next[0] - (t0 + 0x40000000u) >= 2 * mqtt::kReconnectInitialMs);
}

SCENARIO("backoff a deliberate disconnect never starts a retry sequence") {
  MqttRigOf<ControlTraits> rig;
  rig.tick(0);
  rig.applianceClient().disconnect();
  CHECK(attempts(rig, rig.applianceClient(), 10, 60000).empty());
}

#define OUTAGE_ONE(W, name, c)                                                     \
  static codeline::scenario::Registrar SCENARIO_CAT(outage_, __COUNTER__)(name, [] { outage<W>(c); });
#define OUTAGE(W, name)                                                                         \
  OUTAGE_ONE(W, name " A: appliance link drop and return, will left UNAVAILABLE", Case::A)       \
  OUTAGE_ONE(W, name " B: appliance link drop, retained store lost", Case::B)                    \
  OUTAGE_ONE(W, name " C: controller link drop and return", Case::C)                             \
  OUTAGE_ONE(W, name " D: broker restart without persistence", Case::D)                          \
  OUTAGE_ONE(W, name " E: broker restart with persistence", Case::E)

OUTAGE(TurnoutCase, "turnout")
OUTAGE(SensorCase, "sensor ")
OUTAGE(MastCase, "mast   ")
OUTAGE(ControlCase, "control")

int main() { return codeline::scenario::run(); }
