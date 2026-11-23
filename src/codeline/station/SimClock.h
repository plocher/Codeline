// Plant time for a simulated station that a person can pause, step and slow
// down, for learning and debugging. Plant time is what StationHost::tick()
// gets; it never goes back and never jumps: while paused it stands still
// (real time that passes then is dropped, so no timer leaps on resume), and it
// moves only as real time scaled by the rate, while running or during a step.
//
// SimClock is the state and the command grammar; it reads no clock itself
// (the caller passes real monotonic milliseconds). ClockChannel puts it on the
// simulation channel:
//
//   {simulation}/clock             commands, unretained, subscribed at QoS 0 (a retained one is dropped):
//                                    PAUSE            plant time stands still; ends a step, the rest of it given up
//                                    RUN              plant time runs; a step under way just goes on running
//                                    STEP <ms>        only while paused or stepping: run for <ms> of plant
//                                                     time (1..3600000) at the current rate, then pause;
//                                                     steps add up to that limit
//                                    RATE <permille>  1..10000; 1000 is real time, 100 ten times slower
//   {simulation}/clock/{station}   each host's clock, retained:
//                                    RUNNING RATE <permille> T <plant ms>     running, or in a step
//                                    PAUSED RATE <permille> T <plant ms>
//                                  published when it changes between RUNNING and PAUSED (so when a
//                                  step ends, however it ends), when the rate changes, and about
//                                  once a real second while plant time moves. The client's will,
//                                  and a stop of the host, leave retained UNAVAILABLE here.
//
// Every host of a layout listens on the one command topic, so they pause and
// step together; each states its own clock. {station} is spelled as on the
// code line (stationTopic()). A command that is not understood, a STEP while
// running and a value out of range change nothing.
//
// The clock's connection is informative: if it is lost the station runs on,
// and ClockChannel connects again on the ReconnectBackoff schedule (in real
// time) and states the clock again.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "codeline/core/Text.h"
#include "codeline/mqtt/IMqttClient.h"
#include "codeline/mqtt/ReconnectBackoff.h"
#include "codeline/station/CodeLineField.h"

namespace codeline::station {

class SimClock {
public:
  static constexpr uint32_t kMaxRate = 10000;  // permille: ten times real time
  static constexpr uint32_t kMaxStepMs = 3600000;

  explicit SimClock(uint32_t plant_start_ms = 0) : plant_ms_(plant_start_ms) {}

  // Plant time stands still (paused, and no step under way).
  bool paused() const { return paused_ && step_left_ms_ == 0; }
  bool stepping() const { return step_left_ms_ != 0; }
  uint32_t rate() const { return rate_; }
  uint32_t plantMs() const { return plant_ms_; }

  // One command payload. False: not understood or refused; nothing changed.
  bool command(std::string_view text) {
    if (text == "PAUSE" || text == "RUN") {
      paused_ = text == "PAUSE";
      step_left_ms_ = 0;  // PAUSE: the rest of a step is given up. RUN: the step becomes running.
      return true;
    }
    uint32_t v = 0;
    if (text.substr(0, 5) == "STEP ") {
      if (!paused_ || !parseUint(text.substr(5), kMaxStepMs, v) || v == 0 || v > kMaxStepMs - step_left_ms_)
        return false;
      step_left_ms_ += v;
      return true;
    }
    if (text.substr(0, 5) == "RATE ") {
      if (!parseUint(text.substr(5), kMaxRate, v) || v == 0) return false;
      rate_ = v;
      return true;
    }
    return false;
  }

  // Plant time now, given real monotonic milliseconds (any origin; rollover safe).
  uint32_t advance(uint32_t real_ms) {
    const uint32_t dt = started_ ? real_ms - last_real_ms_ : 0;
    last_real_ms_ = real_ms;
    started_ = true;
    if (paused()) return plant_ms_;  // the real time that passed is dropped
    permille_ms_ += static_cast<uint64_t>(dt) * rate_;  // the fraction of a millisecond is carried
    uint64_t ms = permille_ms_ / 1000;
    permille_ms_ %= 1000;
    if (paused_) {  // in a step: exactly what is left of it, then paused
      if (ms >= step_left_ms_) {
        ms = step_left_ms_;
        permille_ms_ = 0;
      }
      step_left_ms_ -= static_cast<uint32_t>(ms);
    }
    plant_ms_ += static_cast<uint32_t>(ms);
    return plant_ms_;
  }

  // "PAUSED RATE 1000"; RUNNING while a step is under way.
  std::string mode() const { return std::string(paused() ? "PAUSED" : "RUNNING") + " RATE " + std::to_string(rate_); }
  // "PAUSED RATE 1000 T 12345"
  std::string status() const { return mode() + " T " + std::to_string(plant_ms_); }

private:
  bool paused_ = false;  // with step_left_ms_ != 0: in a step, paused when it ends
  uint32_t rate_ = 1000;
  uint32_t plant_ms_;
  uint32_t step_left_ms_ = 0;
  bool started_ = false;
  uint32_t last_real_ms_ = 0;
  uint64_t permille_ms_ = 0;
};

class ClockChannel final : private mqtt::IMqttHandler {
public:
  ClockChannel(mqtt::IMqttClient& client, std::string_view simulation_channel, std::string_view station,
               uint32_t plant_start_ms)
      : client_(client), clock_(plant_start_ms) {
    commands_ = std::string(simulation_channel) + "/clock";
    state_ = stationTopic(commands_, station);
  }

  const std::string& commandsTopic() const { return commands_; }
  const std::string& stateTopic() const { return state_; }
  const SimClock& clock() const { return clock_; }

  void connect(std::string_view client_id) {
    id_ = std::string(client_id);
    backoff_.setKey(id_);
    wanted_ = true;
    open();
  }

  // Plant time for this pass of the host's loop; publishes the clock when due,
  // and connects again when the connection was lost and a retry is due.
  uint32_t now(uint32_t real_ms) {
    const uint32_t t = clock_.advance(real_ms);
    backoff_.tick(real_ms);
    if (wanted_ && !client_.connected()) {
      if (!backoff_.pending()) backoff_.lost();
      else if (backoff_.due() && !open()) backoff_.failed();
      if (!client_.connected()) return t;
    }
    const std::string mode = clock_.mode();
    const bool first = published_mode_.empty();
    const bool moved = t != published_t_;
    if (first || mode != published_mode_ || (moved && (clock_.paused() || real_ms - published_real_ms_ >= 1000))) {
      client_.publish(state_, clock_.status(), true);
      published_mode_ = mode;
      published_t_ = t;
      published_real_ms_ = real_ms;
    }
    return t;
  }

  // The host stops: the clock reads UNAVAILABLE, as the will would say.
  void shutdown() {
    wanted_ = false;
    if (!client_.connected()) return;
    client_.publish(state_, kUnavailable, true);
    client_.disconnect();
  }

private:
  mqtt::IMqttClient& client_;
  SimClock clock_;
  std::string commands_, state_, id_;
  mqtt::ReconnectBackoff backoff_;
  bool wanted_ = false;  // between connect() and shutdown()
  std::string published_mode_;
  uint32_t published_t_ = 0;
  uint32_t published_real_ms_ = 0;

  // Connects with the will and subscribes; the clock is then stated again
  // (after a loss the will has replaced it).
  bool open() {
    const mqtt::MqttWill will{state_, kUnavailable, true};
    client_.connect(id_, &will);
    if (!client_.connected()) return false;
    backoff_.connected();
    client_.subscribeAtQos(commands_, this, 0);
    published_mode_.clear();
    return true;
  }

  void onMessage(std::string_view topic, std::string_view payload, bool retained) override {
    if (retained || topic != commands_) return;  // a command is never replayed
    clock_.command(payload);
  }
};

}  // namespace codeline::station
