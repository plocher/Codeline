// Two ways to stand up the same simulated turnout for functional scenarios
// (docs/bindings/Cpp.md section 5):
//   DirectRig: TurnoutCore(SimulatedTurnoutDriver)
//   MqttRig:   MqttTurnoutProxy -> InMemoryMockBroker -> MqttTurnoutAdapter
//              -> TurnoutCore(SimulatedTurnoutDriver)
// A scenario talks to turnout() (an ITurnout), injects physics through
// driver(), and moves time only through tick(now_ms).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "codeline/appliance/INvStore.h"
#include "codeline/appliance/TurnoutCore.h"
#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/mqtt/MqttTurnoutAdapter.h"
#include "codeline/mqtt/MqttTurnoutProxy.h"
#include "codeline/mqtt/TurnoutCodec.h"
#include "codeline/sim/SimulatedTurnoutDriver.h"

namespace codeline::testing {

inline constexpr uint32_t kTravelMs = 2000;  // simulated point travel

struct TurnoutRig {
  virtual ~TurnoutRig() = default;
  virtual ITurnout& turnout() = 0;
  virtual SimulatedTurnoutDriver& driver() = 0;
  virtual InMemoryNvStore& nv() = 0;
  virtual void tick(uint32_t now_ms) = 0;
  // A raw operational frame as a separate-flag binding would carry it. The
  // direct rig has no wire, so it runs the same codec before the call.
  virtual void sendRawCommand(std::string_view frame) = 0;
};

class DirectRig final : public TurnoutRig {
public:
  ITurnout& turnout() override { return core_; }
  SimulatedTurnoutDriver& driver() override { return driver_; }
  InMemoryNvStore& nv() override { return nv_; }
  void tick(uint32_t now_ms) override {
    driver_.tick(now_ms);
    core_.tick(now_ms);
  }
  void sendRawCommand(std::string_view frame) override {
    TurnoutCommand c;
    if (mqtt::decodeCommand(frame, c)) core_.setAlignment(c);
  }

private:
  SimulatedTurnoutDriver driver_{kTravelMs};
  InMemoryNvStore nv_;
  TurnoutCore core_{driver_, nv_};
};

class MqttRig final : public TurnoutRig {
public:
  static constexpr const char* kChannel = "/layout/SPCoast/appliances";
  static constexpr const char* kId = "829";

  MqttRig() {
    adapter_.connect();
    proxy_.connect("office");
  }

  ITurnout& turnout() override { return proxy_; }
  SimulatedTurnoutDriver& driver() override { return driver_; }
  InMemoryNvStore& nv() override { return nv_; }
  void tick(uint32_t now_ms) override {
    driver_.tick(now_ms);
    core_.tick(now_ms);
    adapter_.tick(now_ms);
    proxy_.tick(now_ms);
  }
  void sendRawCommand(std::string_view frame) override {
    office_.publish(topic("cmd"), frame, false);
  }

  // MQTT-only access for transport scenarios.
  mqtt::InMemoryMockBroker& broker() { return broker_; }
  mqtt::InMemoryMockBroker::Client& applianceClient() { return appliance_client_; }
  mqtt::InMemoryMockBroker::Client& officeClient() { return office_; }
  mqtt::MqttTurnoutAdapter& adapter() { return adapter_; }
  mqtt::MqttTurnoutProxy& proxy() { return proxy_; }
  TurnoutCore& core() { return core_; }
  std::string topic(std::string_view suffix) const {
    return std::string(adapter_.applianceTopic()) + "/" + std::string(suffix);
  }

private:
  mqtt::InMemoryMockBroker broker_;
  SimulatedTurnoutDriver driver_{kTravelMs};
  InMemoryNvStore nv_;
  TurnoutCore core_{driver_, nv_};
  mqtt::InMemoryMockBroker::Client appliance_client_{broker_};
  mqtt::InMemoryMockBroker::Client office_{broker_};
  mqtt::MqttTurnoutAdapter adapter_{appliance_client_, kChannel, kId, core_};
  mqtt::MqttTurnoutProxy proxy_{office_, adapter_.applianceTopic()};
};

// Records everything an ITurnout reports, with copies of transient data.
struct Recorder final : ITurnoutObserver {
  struct Event {
    EventKind kind;
    FaultType fault;
    std::string detail;
    AttrMap data;
  };
  std::vector<TurnoutState> states;
  std::vector<Event> events;
  std::string timeline;  // both, in arrival order: "NORMAL Config IN_TRANSIT Fault "

  void onStateChanged(TurnoutState s) override {
    states.push_back(s);
    timeline += std::string(nameOf(s)) + " ";
  }
  void onEvent(const ApplianceEvent& ev) override {
    Event e{ev.kind, ev.fault, std::string(ev.detail), AttrMap{}};
    if (ev.data) e.data = *ev.data;
    events.push_back(e);
    timeline += std::string(kEventKindNames[static_cast<uint8_t>(ev.kind)]) + " ";
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

}  // namespace codeline::testing
