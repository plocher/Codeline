// Two ways to stand up the same simulated SENSOR, SIGNALMAST, CONTROL or LAMP for
// functional scenarios (docs/bindings/Cpp.md section 5), as TurnoutRig.h
// does for TURNOUT:
//   DirectRigOf<T>: Core(SimulatedDriver)
//   MqttRigOf<T>:   Proxy -> InMemoryMockBroker -> Adapter -> Core(SimulatedDriver)
// A scenario talks to app() (the typed interface), injects physics through
// driver(), and moves time only through tick(now_ms).
//
// StateRecorder<T> records what an appliance reports; its timeline spells
// each state the way /state carries it on the wire.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "codeline/appliance/ApplianceHealth.h"
#include "codeline/appliance/ControlCore.h"
#include "codeline/appliance/INvStore.h"
#include "codeline/appliance/LampCore.h"
#include "codeline/appliance/MastCore.h"
#include "codeline/appliance/SensorCore.h"
#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/mqtt/MqttControl.h"
#include "codeline/mqtt/MqttLamp.h"
#include "codeline/mqtt/MqttMast.h"
#include "codeline/mqtt/MqttSensor.h"
#include "codeline/sim/SimulatedControlDriver.h"
#include "codeline/sim/SimulatedLampDriver.h"
#include "codeline/sim/SimulatedMastDriver.h"
#include "codeline/sim/SimulatedSensorDriver.h"

namespace codeline::testing {

inline constexpr const char* kChannel = "/layout/SPCoast/appliances";
inline constexpr uint32_t kHeadMoveMs = 300;  // simulated searchlight mechanism travel

// ---- per-appliance traits ----

struct SensorTraits {
  using Interface = ISensor;
  using Observer = ISensorObserver;
  using State = bool;
  using Core = SensorCore;
  using Driver = SimulatedSensorDriver;
  using Proxy = mqtt::MqttSensorProxy;
  using Adapter = mqtt::MqttSensorAdapter;
  static constexpr const char* kId = "829T1";
  static Driver makeDriver() { return Driver(); }
  static std::string text(bool s) { return std::string(mqtt::encodeSensorState(s).view()); }
  static void applyRaw(ISensor&, std::string_view) {}  // no commands
};

struct MastTraits {
  using Interface = IMast;
  using Observer = IMastObserver;
  using State = MastState;
  using Core = MastCore;
  using Driver = SimulatedMastDriver;
  using Proxy = mqtt::MqttMastProxy;
  using Adapter = mqtt::MqttMastAdapter;
  static constexpr const char* kId = "830WAB";
  static Driver makeDriver() { return Driver(kHeadMoveMs); }
  static std::string text(MastState s) { return std::string(mqtt::encodeMastState(s).view()); }
  static void applyRaw(IMast& m, std::string_view frame) {
    mqtt::MastCommand c;
    if (!mqtt::decodeMastCommand(frame, c)) return;
    if (c.has_indication) m.setIndication(c.indication, c.lit);
    else m.setLit(c.lit);
  }
};

struct ControlTraits {
  using Interface = IControl;
  using Observer = IControlObserver;
  using State = BitVector;
  using Core = ControlCore;
  using Driver = SimulatedControlDriver;
  using Proxy = mqtt::MqttControlProxy;
  using Adapter = mqtt::MqttControlAdapter;
  static constexpr const char* kId = "lock:827";
  static Driver makeDriver() { return Driver(); }
  static std::string text(BitVector s) { return s.count ? std::string(mqtt::encodeSwitches(s).view()) : "-"; }
  static void applyRaw(IControl& c, std::string_view frame) {
    BitVector lamps;
    if (mqtt::decodeLamps(frame, lamps)) c.setLamps(lamps);
  }
};

struct LampTraits {
  using Interface = ILamp;
  using Observer = ILampObserver;
  using State = bool;
  using Core = LampCore;
  using Driver = SimulatedLampDriver;
  using Proxy = mqtt::MqttLampProxy;
  using Adapter = mqtt::MqttLampAdapter;
  static constexpr const char* kId = "Corporal:1MC";
  static Driver makeDriver() { return Driver(); }
  static std::string text(bool s) { return std::string(mqtt::encodeOnOff(s).view()); }
  static void applyRaw(ILamp& l, std::string_view frame) {
    bool on = false;
    if (mqtt::decodeOnOff(frame, on)) l.setOn(on);
  }
};

// ---- rigs ----

template <class T>
struct RigOf {
  virtual ~RigOf() = default;
  virtual typename T::Interface& app() = 0;
  virtual typename T::Driver& driver() = 0;
  virtual InMemoryNvStore& nv() = 0;
  virtual void tick(uint32_t now_ms) = 0;
  // A raw /cmd frame. The direct rig has no wire, so it runs the same codec before the call.
  virtual void sendRawCommand(std::string_view frame) = 0;
  virtual bool isMqtt() const = 0;
};

template <class T>
class DirectRigOf final : public RigOf<T> {
public:
  typename T::Interface& app() override { return core_; }
  typename T::Driver& driver() override { return driver_; }
  InMemoryNvStore& nv() override { return nv_; }
  void tick(uint32_t now_ms) override {
    driver_.tick(now_ms);
    core_.tick(now_ms);
  }
  void sendRawCommand(std::string_view frame) override { T::applyRaw(core_, frame); }
  bool isMqtt() const override { return false; }

private:
  typename T::Driver driver_ = T::makeDriver();
  InMemoryNvStore nv_;
  typename T::Core core_{driver_, nv_};
};

template <class T>
class MqttRigOf final : public RigOf<T> {
public:
  MqttRigOf() {
    adapter_.connect();
    proxy_.connect("office");
  }

  typename T::Interface& app() override { return proxy_; }
  typename T::Driver& driver() override { return driver_; }
  InMemoryNvStore& nv() override { return nv_; }
  void tick(uint32_t now_ms) override {
    driver_.tick(now_ms);
    core_.tick(now_ms);
    adapter_.tick(now_ms);
    proxy_.tick(now_ms);
  }
  void sendRawCommand(std::string_view frame) override { office_.publish(topic("cmd"), frame, false); }
  bool isMqtt() const override { return true; }

  // MQTT-only access for transport scenarios.
  mqtt::InMemoryMockBroker& broker() { return broker_; }
  mqtt::InMemoryMockBroker::Client& applianceClient() { return appliance_client_; }
  mqtt::InMemoryMockBroker::Client& officeClient() { return office_; }
  typename T::Adapter& adapter() { return adapter_; }
  typename T::Proxy& proxy() { return proxy_; }
  typename T::Core& core() { return core_; }
  std::string topic(std::string_view suffix) const {
    return std::string(adapter_.applianceTopic()) + "/" + std::string(suffix);
  }
  const std::string* retained(std::string_view suffix) const { return broker_.retained(topic(suffix)); }
  // Whether the broker carried `payload` on {appliance}/<suffix> with the given retain flag.
  bool sent(std::string_view suffix, std::string_view payload, bool retained) const {
    const std::string t = topic(suffix);
    for (const mqtt::InMemoryMockBroker::Message& m : broker_.log()) {
      if (m.topic == t && m.payload == payload && m.retained == retained) return true;
    }
    return false;
  }

private:
  mqtt::InMemoryMockBroker broker_;
  typename T::Driver driver_ = T::makeDriver();
  InMemoryNvStore nv_;
  typename T::Core core_{driver_, nv_};
  mqtt::InMemoryMockBroker::Client appliance_client_{broker_};
  mqtt::InMemoryMockBroker::Client office_{broker_};
  typename T::Adapter adapter_{appliance_client_, kChannel, T::kId, core_};
  typename T::Proxy proxy_{office_, adapter_.applianceTopic()};
};

// ---- recorder ----

template <class T>
struct StateRecorder final : T::Observer {
  struct Event {
    EventKind kind;
    FaultType fault;
    std::string detail;
    AttrMap data;
    uint8_t channel;
    bool pressed;
  };
  std::vector<typename T::State> states;
  std::vector<Event> events;
  std::string timeline;  // both, in arrival order: "APPROACH Config (ACTIVE) Fault BTN1 "

  void onStateChanged(typename T::State s) override {
    states.push_back(s);
    timeline += T::text(s) + " ";
  }
  void onEvent(const ApplianceEvent& ev) override {
    Event e{ev.kind, ev.fault, std::string(ev.detail), AttrMap{}, ev.channel, ev.pressed};
    if (ev.data) e.data = *ev.data;
    events.push_back(e);
    if (ev.kind == EventKind::Button) {
      timeline += std::string(ev.pressed ? "" : "(") + "BTN" + std::to_string(ev.channel) + (ev.pressed ? "" : ")") + " ";
    } else {
      timeline += std::string(kEventKindNames[static_cast<uint8_t>(ev.kind)]) + " ";
    }
  }

  void clear() {
    states.clear();
    events.clear();
    timeline.clear();
  }
  int count(EventKind k) const {
    int n = 0;
    for (const Event& e : events) n += e.kind == k;
    return n;
  }
  const Event* last(EventKind k) const {
    for (size_t i = events.size(); i-- > 0;) {
      if (events[i].kind == k) return &events[i];
    }
    return nullptr;
  }
  // Raw value of `path` in the most recent Config / Attributes event, or UINT32_MAX.
  uint32_t lastValue(EventKind k, std::string_view path) const {
    const Event* e = last(k);
    AttrValue v;
    return e && e->data.get(path, v) ? v.raw : UINT32_MAX;
  }
};

// A controller binding as the engine has one: health from the appliance's
// reports, attributes asked for when the tracker says so.
template <class T>
struct HealthBinding final : T::Observer {
  ApplianceHealth health;
  void onStateChanged(typename T::State) override { health.onState(); }
  void onEvent(const ApplianceEvent& ev) override { health.onEvent(ev); }
};

template <class T>
void scan(RigOf<T>& rig, HealthBinding<T>& b, uint32_t now_ms) {
  rig.tick(now_ms);
  if (b.health.attributesDue(now_ms)) rig.app().exportAttributes();
}

}  // namespace codeline::testing
