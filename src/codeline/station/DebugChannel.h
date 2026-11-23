// One station's debug channel: what its vital logic just did and why, and what
// the plant holds now, as text for a person (mosquitto_sub, or the panel page's
// debug pane). Informative only: no office and no field logic may read it, and
// nothing here feeds back into the plant.
//
//   {station}/debug         one line per event, unretained (QoS 0):
//                             <plant ms> <text>
//   {station}/debug/plant   the plant now, retained, one line per route, switch,
//                           lock, signal lever, mast, track circuit, contact and
//                           maintainer call;
//                           republished only when its text changes. The client's
//                           will, and a clean stop, leave retained UNAVAILABLE
//                           here (as on {station}/indications), so a dead host's
//                           snapshot never looks live.
//
// {station} is the code line's (CodeLineField.h stationTopic()).
//
// The connection is informative too: if it is lost the station runs on, and
// the channel connects again on the ReconnectBackoff schedule (in tick time)
// and publishes the snapshot again. Lines said meanwhile are not kept.
//
// Event lines:
//   control frame applied | vital ignored: <hazard> | malformed: <what> | out of sync: <ids>
//   signal <lever> <LEFT|RIGHT> demand dropped, nothing cleared: route <r>: <hazard>; ...
//   signal <mast> put to STOP, route <r> dropped: <hazard>
//   switch <s> demand <pos> dropped: locked by <what>
//   route <r> <State> -> <State>: <what that step means>      (time locking starts and expires here)
//   switch <s> travel supervision started | ended | expired ...
//   switch <s> reports <pos>
//   lock <l> <state> -> <state> ...                           (the withdrawal timer starts and expires here)
//   lock <l> request button pressed | released
//   track circuit <c> | contact <c> <state> -> <state>
//   maintainer call <c> <state> -> <state>
//   health <kind> <name> <HEALTH> -> <HEALTH>
//
// Every "why" (a hazard) is the engine's own, recorded where it refused
// (engine::Plant lastRefusal(), takeNotes()); this file adds none. The rest is
// read through the plant's const accessors and compared with the last tick.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "codeline/core/Text.h"
#include "codeline/engine/Plant.h"
#include "codeline/mqtt/IMqttClient.h"
#include "codeline/mqtt/ReconnectBackoff.h"
#include "codeline/station/CodeLineField.h"

namespace codeline::station {

class DebugChannel {
public:
  DebugChannel(mqtt::IMqttClient& client, engine::Plant& plant, std::string_view codeline_channel,
               std::string_view station)
      : client_(client), plant_(plant) {
    events_topic_ = stationTopic(codeline_channel, station) + "/debug";
    plant_topic_ = events_topic_ + "/plant";
  }

  const std::string& eventsTopic() const { return events_topic_; }
  const std::string& plantTopic() const { return plant_topic_; }

  void connect(std::string_view client_id) {
    id_ = std::string(client_id);
    backoff_.setKey(id_);
    wanted_ = true;
    open();
  }

  // The host stops: the snapshot reads UNAVAILABLE, as the will would say.
  void shutdown() {
    wanted_ = false;
    if (!client_.connected()) return;
    client_.publish(plant_topic_, kUnavailable, true);
    client_.disconnect();
  }

  // One event line, stamped with the last tick's time.
  void say(std::string_view text) { client_.publish(events_topic_, std::to_string(now_ms_) + " " + std::string(text), false); }

  // What the code line field did with a control frame (CodeLineField::onFrame).
  void controlFrame(CodeLineField::Outcome outcome, const std::string& detail) {
    const char* what = "applied";
    switch (outcome) {
      case CodeLineField::Outcome::Applied: break;
      case CodeLineField::Outcome::VitalIgnored: what = "vital ignored"; break;
      case CodeLineField::Outcome::Malformed: what = "malformed"; break;
      case CodeLineField::Outcome::OutOfSync: what = "out of sync"; break;
    }
    say(std::string("control frame ") + what + (detail.empty() ? "" : ": " + detail));
  }

  // The snapshot again, if its text changed: after each tick, and after a
  // control frame (which changes stored demands between ticks, and is the only
  // thing that does while plant time is paused).
  void refresh() {
    std::string snap = snapshot();
    if (snap == published_ || !client_.connected()) return;
    client_.publish(plant_topic_, snap, true);
    published_.swap(snap);
  }

  // After each plant tick.
  void tick(uint32_t now_ms) {
    now_ms_ = now_ms;
    backoff_.tick(now_ms);
    if (wanted_ && !client_.connected()) {
      if (!backoff_.pending()) backoff_.lost();
      else if (backoff_.due() && !open()) backoff_.failed();
    }
    std::vector<std::string> now;
    observe(now);
    // Causes before effects: what the appliances reported, then what the
    // engine dropped and why, then the routes' steps.
    const bool compare = now.size() == seen_.size();  // the first tick only records
    for (size_t i = 0; compare && i < now.size(); ++i)
      if (now[i] != seen_[i] && subjects_[i].kind != Kind::Route) change(i, seen_[i], now[i]);
    for (const std::string& n : plant_.takeNotes()) say(n);
    for (size_t i = 0; compare && i < now.size(); ++i)
      if (now[i] != seen_[i] && subjects_[i].kind == Kind::Route) change(i, seen_[i], now[i]);
    seen_.swap(now);
    refresh();
  }

  // The plant now, as published on {station}/debug/plant.
  std::string snapshot() const {
    using engine::Route;
    std::string out;
    for (const Route& r : plant_.routes()) {
      out += "route " + r.name() + " " + Route::stateName(r.state());
      const std::string mast = r.mast() ? r.mast()->name() : std::string("?");
      switch (r.state()) {
        case Route::State::Idle: break;
        case Route::State::Cleared:
          out += ": mast " + mast + " demanded " + nameOf(r.mast()->demanded()) + ", shows " +
                 nameOf(r.mast()->confirmed());
          break;
        case Route::State::Traversing: {
          std::string held, waiting;
          for (size_t i = 0; i < r.switches().size(); ++i)
            if (!r.sectionReleased(i)) held += (held.empty() ? "" : ",") + r.switches()[i].sw->name();
          for (const engine::TrackCircuit* b : r.blocks())
            if (!b->clear()) waiting += (waiting.empty() ? "" : ",") + b->name();
          out += ": train on the route";
          if (!held.empty()) out += ", holds switches " + held;
          if (!waiting.empty()) out += ", until " + waiting + " clear";
          break;
        }
        case Route::State::Releasing: out += ": locked until mast " + mast + " is proven at STOP"; break;
        case Route::State::Timing:
          out += ": time locking " + seconds(timeLockingMs(r), r.terElapsedMs()) + " remaining";
          break;
      }
      out += "\n";
    }
    for (const engine::Switch& sw : plant_.switches()) {
      const bool power = sw.kind() == engine::Switch::Kind::Power;
      out += "switch " + sw.name() + (power ? " power " : " hand ") + positionName(sw.reported());
      if (!sw.healthy()) out += std::string(" (") + healthName(sw.applianceHealth()) + ")";
      if (sw.settling())
        out += std::string(" moving to ") + positionName(sw.demand()) + ", travel supervision " +
               seconds(sw.travelTimeoutMs(), sw.travelElapsedMs()) + " remaining";
      else if (power && sw.timedOut())
        out += std::string(" did not reach ") + positionName(sw.demand());
      std::string by = plant_.lockedBy(sw);
      if (sw.electricLock() != nullptr && sw.electricLock()->provenLocked())
        by = "lock " + sw.electricLock()->name() + (by.empty() ? "" : ", " + by);
      out += by.empty() ? " free" : " locked by " + by;
      out += "\n";
    }
    for (const engine::ElectricLock& l : plant_.electricLocks()) {
      out += "lock " + l.name() + " " + lockState(l);
      if (!l.healthy()) out += std::string(" (") + healthName(l.applianceHealth()) + ")";
      else if (lockState(l) == std::string("withdrawing"))
        out += ", proven locked in " + seconds(l.withdrawalMs(), l.withdrawalElapsedMs());
      if (l.requested()) out += ", request button held";
      out += "\n";
    }
    for (const engine::SignalLever& lever : plant_.levers()) {
      out += "lever " + lever.name() + " " + directionName(lever.active());
      if (lever.pending()) out += " (nothing cleared yet)";
      bool timing = false, awaiting = false;
      uint32_t elapsed = UINT32_MAX;  // of the route that has longest to run
      for (const Route& r : plant_.routes()) {
        if (r.lever() != &lever) continue;
        awaiting = awaiting || r.state() == Route::State::Releasing;
        if (r.state() != Route::State::Timing) continue;
        timing = true;
        if (r.terElapsedMs() < elapsed) elapsed = r.terElapsedMs();
      }
      if (timing) out += ", time locking " + seconds(lever.timeLockingMs(), elapsed) + " remaining";
      if (awaiting) out += ", time locking awaits proof of STOP";
      out += "\n";
    }
    for (const engine::SignalMast& m : plant_.masts()) {
      out += "mast " + m.name() + " ";
      if (!m.healthy()) out += std::string("unhealthy (") + healthName(m.applianceHealth()) + ")";
      else out += nameOf(m.confirmed());
      if (m.demanded() != m.confirmed() || !m.healthy()) out += std::string(", demanded ") + nameOf(m.demanded());
      out += "\n";
    }
    for (const engine::TrackCircuit& tc : plant_.trackCircuits()) out += "circuit " + tc.name() + " " + circuitState(tc) + "\n";
    for (const engine::TrackCircuit& c : plant_.contacts()) out += "contact " + c.name() + " " + contactState(c) + "\n";
    // The lamp as it reports, then the stored CALL control where the two differ.
    for (const engine::MaintainerCall& c : plant_.maintainerCalls()) {
      out += "call " + c.name() + " " + callState(c);
      if (c.called() != c.lit()) out += c.called() ? ", called" : ", not called";
      out += "\n";
    }
    return out;
  }

private:
  mqtt::IMqttClient& client_;
  engine::Plant& plant_;
  std::string events_topic_, plant_topic_, id_;
  mqtt::ReconnectBackoff backoff_;
  bool wanted_ = false;  // between connect() and shutdown()
  uint32_t now_ms_ = 0;
  std::string published_;
  std::vector<std::string> seen_;  // last tick's observations, in observe() order

  // Connects with the will. The snapshot is then published again (after a
  // loss the will has replaced it).
  bool open() {
    const mqtt::MqttWill will{plant_topic_, kUnavailable, true};
    client_.connect(id_, &will);
    if (!client_.connected()) return false;
    backoff_.connected();
    published_.clear();
    return true;
  }

  static const char* healthName(Health h) {
    switch (h) {
      case Health::Ok: return "OK";
      case Health::Unproven: return "UNPROVEN";
      case Health::Unavailable: return "UNAVAILABLE";
      case Health::Offline: return "OFFLINE";
      case Health::Faulted: return "FAULTED";
    }
    return "?";
  }

  // "12 s": what is left of a running timer, rounded up to whole seconds (so
  // the snapshot changes once a second, not every tick).
  static std::string seconds(uint32_t total_ms, uint32_t elapsed_ms) {
    const uint32_t left = elapsed_ms >= total_ms ? 0 : total_ms - elapsed_ms;
    return std::to_string((left + 999) / 1000) + " s";
  }

  static uint32_t timeLockingMs(const engine::Route& r) { return r.lever() ? r.lever()->timeLockingMs() : 0; }

  static const char* lockState(const engine::ElectricLock& l) {
    if (!l.healthy()) return "unhealthy";
    if (l.open()) return "open";
    if (l.granted()) return "release granted";
    if (l.provenLocked()) return "locked";
    return l.withdrawing() ? "withdrawing" : "not proven locked";
  }
  static std::string circuitState(const engine::TrackCircuit& tc) {
    if (!tc.healthy()) return std::string("unhealthy (") + healthName(tc.applianceHealth()) + ")";
    return tc.clear() ? "clear" : "occupied";
  }
  static std::string contactState(const engine::TrackCircuit& c) {
    if (!c.healthy()) return std::string("unhealthy (") + healthName(c.applianceHealth()) + ")";
    return c.clear() ? "open" : "closed";
  }
  static std::string callState(const engine::MaintainerCall& c) {
    if (!c.healthy()) return std::string("unhealthy (") + healthName(c.applianceHealth()) + ")";
    return c.lit() ? "lit" : "dark";
  }
  static const char* travel(const engine::Switch& sw) {
    if (sw.settling()) return "running";
    return sw.timedOut() ? "expired" : "idle";
  }

  // What is compared from tick to tick, a fixed number of entries in a fixed
  // order: the health of every appliance; per circuit and contact its state;
  // per switch its reported position, then its travel supervision; per lock
  // its state, then its request button; per maintainer call its lamp; per
  // route its state.
  enum class Kind : uint8_t { Route, Travel, Position, Lock, Request, Circuit, Contact, Call, Health };
  struct Subject {
    Kind kind;
    size_t index;
    std::string name;  // for Health: "<kind> <name>"
  };
  std::vector<Subject> subjects_;

  void observe(std::vector<std::string>& now) {
    const bool first = subjects_.empty();
    auto put = [&](Kind k, size_t i, const std::string& name, std::string value) {
      if (first) subjects_.push_back(Subject{k, i, name});
      now.push_back(std::move(value));
    };
    for (const engine::TrackCircuit& tc : plant_.trackCircuits())
      put(Kind::Health, 0, "track circuit " + tc.name(), healthName(tc.applianceHealth()));
    for (const engine::TrackCircuit& c : plant_.contacts())
      put(Kind::Health, 0, "contact " + c.name(), healthName(c.applianceHealth()));
    for (const engine::Switch& sw : plant_.switches())
      put(Kind::Health, 0, "switch " + sw.name(), healthName(sw.applianceHealth()));
    for (const engine::ElectricLock& l : plant_.electricLocks())
      put(Kind::Health, 0, "lock " + l.name(), healthName(l.applianceHealth()));
    for (const engine::SignalMast& m : plant_.masts())
      put(Kind::Health, 0, "mast " + m.name(), healthName(m.applianceHealth()));
    for (const engine::MaintainerCall& c : plant_.maintainerCalls())
      if (c.bound()) put(Kind::Health, 0, "maintainer call " + c.name(), healthName(c.applianceHealth()));
    size_t i = 0;
    for (const engine::TrackCircuit& tc : plant_.trackCircuits()) put(Kind::Circuit, i++, tc.name(), circuitState(tc));
    i = 0;
    for (const engine::TrackCircuit& c : plant_.contacts()) put(Kind::Contact, i++, c.name(), contactState(c));
    i = 0;
    for (const engine::Switch& sw : plant_.switches()) {
      put(Kind::Position, i, sw.name(), positionName(sw.reported()));
      put(Kind::Travel, i++, sw.name(), travel(sw));
    }
    i = 0;
    for (const engine::ElectricLock& l : plant_.electricLocks()) {
      put(Kind::Lock, i, l.name(), lockState(l));
      put(Kind::Request, i++, l.name(), l.requested() ? "pressed" : "released");
    }
    i = 0;
    for (const engine::MaintainerCall& c : plant_.maintainerCalls()) put(Kind::Call, i++, c.name(), callState(c));
    i = 0;
    for (const engine::Route& r : plant_.routes()) put(Kind::Route, i++, r.name(), engine::Route::stateName(r.state()));
  }

  // What a route's step from one state to the next means (Route.h, Plant.h evaluate()).
  std::string routeStep(const engine::Route& r, const std::string& was) const {
    using State = engine::Route::State;
    const std::string mast = r.mast() ? r.mast()->name() : std::string("?");
    switch (r.state()) {
      case State::Cleared: return "signal " + mast + " cleared, route locked";
      case State::Traversing:
        return "a train accepted signal " + mast +
               (r.blocks().empty() ? std::string() : " (" + r.blocks().front()->name() + " occupied)") +
               ", sectional release";
      case State::Releasing: return "route locked until mast " + mast + " is proven at STOP";
      case State::Timing:
        return "time locking started, " + std::to_string(timeLockingMs(r)) + " ms on signal lever " +
               (r.lever() ? r.lever()->name() : std::string("?")) + " (mast " + mast +
               " proven at STOP, a train may be approaching)";
      case State::Idle:
        if (was == "Timing") return "time locking expired, route released";
        if (was == "Releasing") return "mast " + mast + " proven at STOP and no train approaching, route released";
        if (was == "Traversing") return "the train is off the route, route released";
        return "route released";
    }
    return "";
  }

  void change(size_t i, const std::string& was, const std::string& is) {
    const Subject& s = subjects_[i];
    switch (s.kind) {
      case Kind::Route:
        say("route " + s.name + " " + was + " -> " + is + ": " + routeStep(plant_.routes()[s.index], was));
        break;
      case Kind::Travel: {
        const engine::Switch& sw = plant_.switches()[s.index];
        const std::string limit = std::to_string(sw.travelTimeoutMs()) + " ms";
        if (is == "running")
          say("switch " + s.name + " travel supervision started: to " + positionName(sw.demand()) + " within " + limit);
        else if (is == "expired")
          say("switch " + s.name + " travel supervision expired: not at " + positionName(sw.demand()) + " after " +
              limit + " (it reports " + positionName(sw.reported()) + ")");
        else if (was == "running")
          say("switch " + s.name + " travel supervision ended: it reports " + positionName(sw.reported()));
        break;
      }
      case Kind::Position: say("switch " + s.name + " reports " + is + " (was " + was + ")"); break;
      case Kind::Lock: {
        const engine::ElectricLock& l = plant_.electricLocks()[s.index];
        std::string line = "lock " + s.name + " " + was + " -> " + is;
        if (is == "withdrawing")
          line += ": lock withdrawal timer started, " + std::to_string(l.withdrawalMs()) + " ms";
        else if (was == "withdrawing" && is == "locked")
          line += ": lock withdrawal timer expired, proven locked";
        say(line);
        break;
      }
      case Kind::Request: say("lock " + s.name + " request button " + is); break;
      case Kind::Circuit: say("track circuit " + s.name + " " + was + " -> " + is); break;
      case Kind::Contact: say("contact " + s.name + " " + was + " -> " + is); break;
      case Kind::Call: say("maintainer call " + s.name + " " + was + " -> " + is); break;
      case Kind::Health: say("health " + s.name + " " + was + " -> " + is); break;
    }
  }
};

}  // namespace codeline::station
