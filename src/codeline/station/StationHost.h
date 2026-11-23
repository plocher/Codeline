// One station, simulated end to end, behind both interfaces:
//
//   office ──code line MQTT──▶ CodeLineField ─▶ engine::Plant
//                                                   │ ITurnout / ISensor / IMast / IControl
//                                                   ▼
//                                        Mqtt*Proxy ──MQTT──▶ Mqtt*Adapter ─▶ *Core ─▶ Simulated*Driver
//
// Everything is built from a station file (StationFile.h); nothing about a
// particular station is written here. Every MQTT client comes from a factory,
// so the same host runs over InMemoryMockBroker (scenarios, injected time) or
// over a real broker (MosquittoClient.h, wall-clock time; src/codeline_host.cpp).
//
// The physical world (a train shunting a track circuit, a crew throwing a
// hand switch, opening the lock or pressing its request button) arrives on a
// simulation channel, outside the appliance and code line channels:
//
//   {simulation}/sensor/{id}    SHUNT | (SHUNT)        a wheelset bridges / leaves the circuit
//   {simulation}/turnout/{id}   NORMAL | REVERSE       the points are thrown by hand
//   {simulation}/control/{id}   SW<k> | (SW<k>)        a contact is closed / opened
//                               BTN<k> | (BTN<k>)      a button is pressed / released
//   {simulation}/{role}/{id}    (POWER) | POWER        any appliance loses power (its /state reads
//                                                      UNAVAILABLE, as its will would say) / is powered again
//
// {simulation} is the station file's simulation channel. Retained simulation
// messages are dropped, as commands are. HostConfig::initial applies the same
// payloads before power-on (the plant as found at start).
//
// tick(now_ms): drivers, cores and adapters, then the plant, then the code
// line (which publishes indications that changed). Time enters only here.
// The code line field connects and subscribes in the first tick, after the
// plant's first scan (which finalizes it): no control frame is read by a plant
// that has not started.
//
// shutdown(): the station's indications and every appliance's /state read
// UNAVAILABLE, retained, as their wills would on an abrupt loss.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codeline/appliance/ControlCore.h"
#include "codeline/appliance/INvStore.h"
#include "codeline/appliance/MastCore.h"
#include "codeline/appliance/SensorCore.h"
#include "codeline/appliance/TurnoutCore.h"
#include "codeline/core/Text.h"
#include "codeline/engine/Engine.h"
#include "codeline/generated/MqttTables.h"
#include "codeline/mqtt/IMqttClient.h"
#include "codeline/mqtt/MqttControl.h"
#include "codeline/mqtt/MqttMast.h"
#include "codeline/mqtt/MqttSensor.h"
#include "codeline/mqtt/MqttTurnoutAdapter.h"
#include "codeline/mqtt/MqttTurnoutProxy.h"
#include "codeline/sim/SimulatedControlDriver.h"
#include "codeline/sim/SimulatedMastDriver.h"
#include "codeline/sim/SimulatedSensorDriver.h"
#include "codeline/sim/SimulatedTurnoutDriver.h"
#include "codeline/station/CodeLineField.h"
#include "codeline/station/StationFile.h"

namespace codeline::station {

struct HostConfig {
  uint32_t time_locking_ms = 0;     // 0: as the station file says
  std::string simulation_channel;  // empty: as the station file says
  // Simulated physics, not plant facts.
  uint32_t turnout_travel_ms = 1000;  // inside the 2 s travel supervision
  uint32_t head_move_ms = 300;
  // Simulation payloads applied before power-on: {"turnout/829", "REVERSE"}.
  std::vector<std::pair<std::string, std::string>> initial;
};

using ClientFactory = std::function<std::unique_ptr<mqtt::IMqttClient>(const std::string& client_id)>;

class StationHost final : private mqtt::IMqttHandler {
public:
  StationHost(const StationSpec& spec, HostConfig config, ClientFactory factory)
      : spec_(spec), config_(std::move(config)), factory_(std::move(factory)), plant_(spec.chart.station) {
    if (config_.simulation_channel.empty()) config_.simulation_channel = spec_.simulation_channel;
  }

  StationHost(const StationHost&) = delete;
  StationHost& operator=(const StationHost&) = delete;

  // Builds and connects everything. False (with a reason) if the station
  // file and the plant disagree; nothing should be ticked then.
  bool start(std::string& error) {
    if (!buildAppliances(error) || !buildPlant(error)) return false;
    if (plant_.configurationRefused()) return error = "the plant refused its configuration", false;
    field_client_ = factory_(spec_.chart.station + "-field");
    field_ = std::make_unique<CodeLineField>(*field_client_, spec_.chart, plant_, spec_.codeline_channel);
    if (!field_->verify(error)) return false;
    for (const auto& [suffix, payload] : config_.initial) {
      if (!simulate(suffix, payload)) return error = "initial " + suffix + " " + payload + " is not understood", false;
    }
    for (auto& u : sensors_) connectUnit(*u);
    for (auto& u : turnouts_) connectUnit(*u);
    for (auto& u : controls_) connectUnit(*u);
    for (auto& u : masts_) connectUnit(*u);
    sim_client_ = factory_(spec_.chart.station + "-simulation");
    sim_client_->connect(spec_.chart.station + "-simulation", nullptr);
    for (const std::string& t : simTopics()) sim_client_->subscribe(t, this);
    return true;
  }

  void tick(uint32_t now_ms) {
    for (auto& u : sensors_) u->tick(now_ms);
    for (auto& u : turnouts_) u->tick(now_ms);
    for (auto& u : controls_) u->tick(now_ms);
    for (auto& u : masts_) u->tick(now_ms);
    plant_.tick(now_ms);
    if (!field_connected_) {  // after the plant's first scan (see above)
      field_->connect(spec_.chart.station + "-field");
      field_connected_ = true;
    }
    field_->tick();
  }

  // Clean stop: the station and every appliance say UNAVAILABLE themselves
  // (retained, what their wills say), then every client disconnects. The
  // engine's clients go first, so the plant never reads an appliance's goodbye.
  void shutdown() {
    if (field_) field_->shutdown();
    for (auto& u : sensors_) u->engine_client->disconnect();
    for (auto& u : turnouts_) u->engine_client->disconnect();
    for (auto& u : controls_) u->engine_client->disconnect();
    for (auto& u : masts_) u->engine_client->disconnect();
    for (auto& u : sensors_) goodbye(*u);
    for (auto& u : turnouts_) goodbye(*u);
    for (auto& u : controls_) goodbye(*u);
    for (auto& u : masts_) goodbye(*u);
    if (sim_client_) sim_client_->disconnect();
  }

  engine::Plant& plant() { return plant_; }
  CodeLineField& field() { return *field_; }

  // One simulation payload for "{role}/{id}"; false if not understood.
  bool simulate(std::string_view suffix, std::string_view payload) {
    if (payload == "POWER" || payload == "(POWER)") return power(suffix, payload == "POWER");
    for (auto& u : sensors_) {
      if (suffix != u->suffix()) continue;
      if (payload == "SHUNT" || payload == "(SHUNT)") return u->driver.shunt(payload == "SHUNT"), true;
      return false;
    }
    for (auto& u : turnouts_) {
      if (suffix != u->suffix()) continue;
      if (payload == "NORMAL") return u->driver.handThrow(SimulatedTurnoutDriver::Points::Normal), true;
      if (payload == "REVERSE") return u->driver.handThrow(SimulatedTurnoutDriver::Points::Reverse), true;
      return false;
    }
    for (auto& u : controls_) {
      if (suffix != u->suffix()) continue;
      uint8_t k = 0;
      bool on = false;
      if (indexed(payload, "SW", k, on)) return u->driver.setSwitch(k, on), true;
      if (indexed(payload, "BTN", k, on)) return u->driver.press(k, on), true;
      return false;
    }
    return false;
  }

private:
  template <class Driver, class Core, class Adapter, class Proxy>
  struct Unit {
    using AdapterType = Adapter;
    using ProxyType = Proxy;
    template <class... A>
    explicit Unit(std::string r, std::string i, A&&... args)
        : role(std::move(r)), id(std::move(i)), driver(std::forward<A>(args)...) {}
    std::string role, id;
    Driver driver;
    InMemoryNvStore nv;
    Core core{driver, nv};
    std::unique_ptr<mqtt::IMqttClient> appliance_client, engine_client;
    std::unique_ptr<Adapter> adapter;
    std::unique_ptr<Proxy> proxy;

    std::string suffix() const { return role + "/" + id; }
    void tick(uint32_t now_ms) {
      driver.tick(now_ms);
      core.tick(now_ms);
      adapter->tick(now_ms);
    }
  };
  using SensorUnit = Unit<SimulatedSensorDriver, SensorCore, mqtt::MqttSensorAdapter, mqtt::MqttSensorProxy>;
  using TurnoutUnit = Unit<SimulatedTurnoutDriver, TurnoutCore, mqtt::MqttTurnoutAdapter, mqtt::MqttTurnoutProxy>;
  using ControlUnit = Unit<SimulatedControlDriver, ControlCore, mqtt::MqttControlAdapter, mqtt::MqttControlProxy>;
  using MastUnit = Unit<SimulatedMastDriver, MastCore, mqtt::MqttMastAdapter, mqtt::MqttMastProxy>;

  const StationSpec& spec_;
  HostConfig config_;
  ClientFactory factory_;
  engine::Plant plant_;
  std::vector<std::unique_ptr<SensorUnit>> sensors_;
  std::vector<std::unique_ptr<TurnoutUnit>> turnouts_;
  std::vector<std::unique_ptr<ControlUnit>> controls_;
  std::vector<std::unique_ptr<MastUnit>> masts_;
  std::unique_ptr<mqtt::IMqttClient> field_client_, sim_client_;
  std::unique_ptr<CodeLineField> field_;
  bool field_connected_ = false;

  // ---- appliances ----------------------------------------------------------------

  template <class U>
  void wire(U& u) {
    u.appliance_client = factory_(spec_.appliance_channel + "/" + u.suffix());
    u.engine_client = factory_(spec_.chart.station + "-engine-" + u.role + "-" + u.id);
    u.adapter = std::make_unique<typename U::AdapterType>(*u.appliance_client, spec_.appliance_channel, u.id, u.core);
    u.proxy = std::make_unique<typename U::ProxyType>(*u.engine_client, u.adapter->applianceTopic());
  }
  template <class U>
  void connectUnit(U& u) {
    u.adapter->connect();
    u.proxy->connect(spec_.chart.station + "-engine-" + u.role + "-" + u.id);
  }
  // Power lost: the appliance's /state reads UNAVAILABLE, as its will would
  // say, and it is off the broker. Power back: it connects again and states
  // itself. Before start() (an `initial`), nothing is connected yet: refused.
  bool power(std::string_view suffix, bool on) {
    bool found = false;
    auto apply = [&](auto& units) {
      for (auto& u : units) {
        if (suffix != u->suffix() || !u->adapter) continue;
        found = true;
        if (!on) goodbye(*u);
        else if (!u->appliance_client->connected()) u->adapter->connect();
      }
    };
    apply(sensors_);
    apply(turnouts_);
    apply(controls_);
    apply(masts_);
    return found && sim_client_ != nullptr;
  }

  template <class U>
  static void goodbye(U& u) {
    if (!u.appliance_client->connected()) return;
    u.appliance_client->publish(std::string(u.adapter->applianceTopic()) + "/" + mqtt::kWillTopic, mqtt::kWillPayload,
                                mqtt::kWillRetained);
    u.appliance_client->disconnect();
  }

  static bool roleIs(const std::string& role, const char* expected, std::string& error, const std::string& what) {
    if (role == expected) return true;
    error = what + ": role " + role + ", but its appliance is a " + expected;
    return false;
  }

  // NV for a simulated core, before power-on: a number, or an enum name of the attribute.
  static bool applyNv(INvStore& nv, const AttributeTable& table, const std::vector<NvSetting>& settings,
                      std::string& error, const std::string& what) {
    for (const NvSetting& s : settings) {
      const AttributeInfo* a = table.find(s.path);
      uint32_t v = 0;
      bool ok = a != nullptr && parseUint(s.value, a->max_value, v) && v >= a->min_value;
      if (!ok && a != nullptr && a->enum_names != nullptr) {
        for (uint32_t i = a->min_value; i <= a->max_value && !ok; ++i)
          if (a->enum_names[i] != nullptr && s.value == a->enum_names[i]) ok = true, v = i;
      }
      if (!ok) return error = what + ": cannot store " + s.path + "=" + s.value, false;
      nv.save(s.path, v);
    }
    return true;
  }

  bool buildAppliances(std::string& error) {
    for (const CircuitSpec& c : spec_.circuits) {
      if (!roleIs(c.role, kSensorRole, error, "circuit " + c.name)) return false;
      sensors_.push_back(std::make_unique<SensorUnit>(c.role, c.id));
      wire(*sensors_.back());
    }
    for (const CircuitSpec& c : spec_.contacts) {
      if (!roleIs(c.role, kSensorRole, error, "contact " + c.name)) return false;
      sensors_.push_back(std::make_unique<SensorUnit>(c.role, c.id));
      wire(*sensors_.back());
    }
    for (const SwitchSpec& s : spec_.switches) {
      if (!roleIs(s.role, kTurnoutRole, error, "switch " + s.name)) return false;
      turnouts_.push_back(std::make_unique<TurnoutUnit>(s.role, s.id, config_.turnout_travel_ms));
      wire(*turnouts_.back());
    }
    for (const LockSpec& l : spec_.locks) {
      if (!roleIs(l.role, kControlRole, error, "lock " + l.name)) return false;
      controls_.push_back(std::make_unique<ControlUnit>(l.role, l.id));
      wire(*controls_.back());
      if (!applyNv(controls_.back()->nv, kControlAttributeTable, l.nv, error, "lock " + l.name)) return false;
    }
    for (const MastSpec& m : spec_.masts) {
      if (!roleIs(m.role, kSignalmastRole, error, "mast " + m.name)) return false;
      masts_.push_back(std::make_unique<MastUnit>(m.role, m.id, config_.head_move_ms));
      wire(*masts_.back());
      if (!applyNv(masts_.back()->nv, kSignalmastAttributeTable, m.nv, error, "mast " + m.name)) return false;
    }
    return true;
  }

  // ---- the plant ------------------------------------------------------------------

  template <class U>
  static U* unit(std::vector<std::unique_ptr<U>>& units, const std::string& role, const std::string& id) {
    for (auto& u : units)
      if (u->role == role && u->id == id) return u.get();
    return nullptr;
  }

  engine::TrackCircuit* circuit(const std::string& name, std::string& error, const std::string& what) {
    engine::TrackCircuit* tc = plant_.findTrackCircuit(name);
    if (tc == nullptr) error = what + ": no circuit " + name;
    return tc;
  }

  // Plant::addElectricLock takes an initializer_list; a station file gives a list.
  engine::ElectricLock* addLock(const LockSpec& l, IControl& c, const std::vector<engine::Switch*>& s) {
    switch (s.size()) {
      case 1: return &plant_.addElectricLock(l.name, c, {s[0]});
      case 2: return &plant_.addElectricLock(l.name, c, {s[0], s[1]});
      case 3: return &plant_.addElectricLock(l.name, c, {s[0], s[1], s[2]});
      case 4: return &plant_.addElectricLock(l.name, c, {s[0], s[1], s[2], s[3]});
      default: return nullptr;
    }
  }

  bool buildPlant(std::string& error) {
    for (const CircuitSpec& c : spec_.circuits) plant_.addTrackCircuit(c.name, *unit(sensors_, c.role, c.id)->proxy);
    for (const SwitchSpec& s : spec_.switches) {
      engine::TrackCircuit* os = nullptr;
      if (!s.os.empty() && (os = circuit(s.os, error, "switch " + s.name)) == nullptr) return false;
      ITurnout& t = *unit(turnouts_, s.role, s.id)->proxy;
      engine::Switch& sw = s.power ? plant_.addPowerSwitch(s.name, t, os) : plant_.addHandSwitch(s.name, t, os);
      sw.setTravelTimeoutMs(s.travel_timeout_ms);
    }
    for (const LockSpec& l : spec_.locks) {
      std::vector<engine::Switch*> locked;
      for (const std::string& n : l.switches) {
        engine::Switch* sw = plant_.findSwitch(n);
        if (sw == nullptr || sw->kind() != engine::Switch::Kind::Hand)
          return error = "lock " + l.name + ": " + n + " is no hand switch of the plant", false;
        locked.push_back(sw);
      }
      if (addLock(l, *unit(controls_, l.role, l.id)->proxy, locked) == nullptr)
        return error = "lock " + l.name + ": more switches than this host can bind", false;
    }
    for (const LeverSpec& lv : spec_.levers)
      plant_.addSignalLever(lv.name).setTimeLockingMs(config_.time_locking_ms ? config_.time_locking_ms
                                                                              : lv.time_locking_ms);
    for (const MastSpec& m : spec_.masts) plant_.addMast(m.name, *unit(masts_, m.role, m.id)->proxy);
    for (const std::string& c : spec_.calls) plant_.addMaintainerCall(c);
    for (const CircuitSpec& c : spec_.contacts) plant_.addContact(c.name, *unit(sensors_, c.role, c.id)->proxy);
    for (const std::string& l : spec_.unbound_lamps) plant_.addUnboundLamp(l);
    for (const RouteSpec& rs : spec_.routes) {
      const std::string what = "route " + rs.name;
      engine::SignalMast* mast = plant_.findMast(rs.mast);
      if (mast == nullptr) return error = what + ": no mast " + rs.mast, false;
      SignalIndication ceiling = SignalIndication::Stop;
      for (SignalIndication p : rs.presents) ceiling = engine::mostPermissive(ceiling, p);
      for (SignalIndication c : rs.caps) ceiling = engine::leastPermissive(ceiling, c);
      // A route whose best indication its mast cannot present is no route: the
      // plant would line and lock it, and the mast would never show it.
      bool presentable = false;
      for (SignalIndication p : rs.presents) presentable = presentable || p == ceiling;
      if (!presentable) return error = what + ": its ceiling " + nameOf(ceiling) + " is no indication mast " +
                                       rs.mast + " presents", false;
      engine::Route& r = plant_.addRoute(rs.name);
      r.shows(*mast, ceiling);
      for (size_t i = 0; i < rs.levers.size(); ++i) {
        engine::SignalLever* lever = plant_.findLever(rs.levers[i].first);
        if (lever == nullptr) return error = what + ": no signal lever " + rs.levers[i].first, false;
        if (i == 0) r.governedBy(*lever, rs.levers[i].second);
        else r.alsoRequires(*lever, rs.levers[i].second);
      }
      for (const AlignSpec& a : rs.align) {
        engine::Switch* sw = plant_.findSwitch(a.sw);
        if (sw == nullptr) return error = what + ": no switch " + a.sw, false;
        engine::TrackCircuit* rel = nullptr;
        if (!a.releasing.empty() && (rel = circuit(a.releasing, error, what)) == nullptr) return false;
        r.aligns(*sw, a.pos, rel);
      }
      for (const std::string& c : rs.clears) {
        engine::TrackCircuit* tc = circuit(c, error, what);
        if (tc == nullptr) return false;
        r.clears({tc});
      }
      if (!rs.approach.empty()) {
        engine::TrackCircuit* tc = circuit(rs.approach, error, what);
        if (tc == nullptr) return false;
        r.approachLockedBy(*tc);
      }
    }
    return true;
  }

  // ---- the simulated physical world --------------------------------------------------

  std::vector<std::string> simTopics() const {
    std::vector<std::string> out;
    for (auto& u : sensors_) out.push_back(config_.simulation_channel + "/" + u->suffix());
    for (auto& u : turnouts_) out.push_back(config_.simulation_channel + "/" + u->suffix());
    for (auto& u : controls_) out.push_back(config_.simulation_channel + "/" + u->suffix());
    for (auto& u : masts_) out.push_back(config_.simulation_channel + "/" + u->suffix());
    return out;
  }

  // "SW1" -> (1, true), "(BTN1)" -> (1, false).
  static bool indexed(std::string_view s, std::string_view prefix, uint8_t& k, bool& on) {
    on = !(s.size() > 2 && s.front() == '(' && s.back() == ')');
    if (!on) s = s.substr(1, s.size() - 2);
    if (s.substr(0, prefix.size()) != prefix) return false;
    uint32_t v = 0;
    if (!parseUint(s.substr(prefix.size()), 32, v) || v == 0) return false;
    k = static_cast<uint8_t>(v);
    return true;
  }

  void onMessage(std::string_view topic, std::string_view payload, bool retained) override {
    if (retained) return;  // the physical world is never replayed
    const std::string_view root = config_.simulation_channel;
    if (topic.size() <= root.size() + 1 || topic.substr(0, root.size()) != root) return;
    simulate(topic.substr(root.size() + 1), payload);
  }
};

}  // namespace codeline::station
