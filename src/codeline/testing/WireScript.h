// A wire script: what an office and the physical world send, and every
// message the station must answer with, in order, on the topics it names.
// One script is replayed twice: in process over InMemoryMockBroker with
// injected time (scenarios/corporal_walkthrough.cpp, this header), and over a
// real broker (scenarios/corporal_broker.py, tools/codeline_office.py, which
// reads the same grammar).
//
// Grammar, one directive per line. A topic is <channel>/<rest>, where
// <channel> is appliances, codeline or simulation, read from the station
// file's `channel` records (appliances/turnout/829/cmd ->
// {appliances channel}/turnout/829/cmd):
//
//   initial <role>/<id> <payload>     simulation payload applied before power-on
//   step <text>                       a label for the report
//   await <topic> <payload>           wait until this payload is published on topic
//   begin                             from here on, every message on a watched topic must be scripted
//   retained <topic> <payload>        the broker holds this retained payload now
//   send <topic> <payload>            published (unretained) by the office or the physical world
//   expect <topic> <payload>          the next message on topic; the Walkthrough shows it
//   extra <topic> <payload>           the next message on topic; the Walkthrough does not show it
//   differs <topic> <payload> | <the Walkthrough>    the next message on topic; it differs from the Walkthrough
//   quiet <ms>                        nothing arrives on a watched topic for ms
//   mark <name>                       remember when the last matched message arrived
//   elapsed <name>                    the time since <name> is the time-locking time
//
// Watched topics are those named by expect, extra and differs, and every
// appliance command ({appliances}/+/+/cmd) and every station's indications
// ({codeline}/+/indications): from `begin` on, any message on one of them the
// script does not match fails the run. In process, the matched messages must
// also appear in script order across topics; over a real broker only the order
// on each topic is checked (messages from different clients may reach a
// listener in either order).
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "codeline/core/Text.h"
#include "codeline/mqtt/InMemoryMockBroker.h"

namespace codeline::testing {

// The script's channel names -> topic prefixes (from the station file).
using WireChannels = std::map<std::string, std::string>;

// "appliances/turnout/829/cmd" -> "/layout/SPCoast/appliances/turnout/829/cmd"; "" if no such channel.
inline std::string wireTopic(const WireChannels& channels, const std::string& topic) {
  const size_t slash = topic.find('/');
  if (slash == std::string::npos) return "";
  const auto c = channels.find(topic.substr(0, slash));
  return c == channels.end() ? "" : c->second + topic.substr(slash);
}

// Every appliance command and every station's indications.
inline bool alwaysWatched(const WireChannels& channels, const std::string& topic) {
  auto under = [&topic](const std::string& prefix, int levels, const char* last) {
    if (topic.compare(0, prefix.size() + 1, prefix + "/") != 0) return false;
    const std::string rest = topic.substr(prefix.size() + 1);
    int slashes = 0;
    for (char ch : rest) slashes += ch == '/';
    const size_t tail = rest.rfind('/');
    return slashes == levels && tail != std::string::npos && rest.substr(tail + 1) == last;
  };
  const auto a = channels.find("appliances"), c = channels.find("codeline");
  return (a != channels.end() && under(a->second, 2, "cmd")) || (c != channels.end() && under(c->second, 1, "indications"));
}

struct WireLine {
  int number = 0;
  std::string directive, topic, payload, walkthrough;  // walkthrough: after " | " on a differs line
};

inline bool parseWireScript(const char* path, std::vector<WireLine>& out, std::string& error) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return error = std::string("cannot open ") + path, false;
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
  std::fclose(f);
  int number = 0;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    ++number;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    WireLine w;
    w.number = number;
    size_t sp = line.find(' ');
    w.directive = line.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
    if (w.directive == "begin") {
    } else if (w.directive == "step" || w.directive == "quiet" || w.directive == "mark" || w.directive == "elapsed") {
      w.payload = rest;
    } else {
      sp = rest.find(' ');
      if (sp == std::string::npos) return error = "line " + std::to_string(number) + ": no payload", false;
      w.topic = rest.substr(0, sp);
      w.payload = rest.substr(sp + 1);
      if (w.directive == "differs") {
        const size_t bar = w.payload.find(" | ");
        if (bar == std::string::npos) return error = "line " + std::to_string(number) + ": differs needs ' | '", false;
        w.walkthrough = w.payload.substr(bar + 3);
        w.payload = w.payload.substr(0, bar);
      }
    }
    static const std::set<std::string> kKnown = {"initial", "step", "await", "begin", "retained", "send", "expect",
                                                 "extra", "differs", "quiet", "mark", "elapsed"};
    if (!kKnown.count(w.directive)) return error = "line " + std::to_string(number) + ": unknown " + w.directive, false;
    out.push_back(w);
  }
  return true;
}

// Replays a script over an InMemoryMockBroker. `tick(now)` advances the
// station; `office` publishes for the office and the physical world.
class MockWireRunner {
public:
  MockWireRunner(mqtt::InMemoryMockBroker& broker, mqtt::IMqttClient& office, WireChannels channels,
                 std::function<void(uint32_t)> tick, uint32_t start_ms, uint32_t time_locking_ms)
      : broker_(broker), office_(office), channels_(std::move(channels)), tick_(std::move(tick)), now_(start_ms),
        time_locking_ms_(time_locking_ms) {}

  static constexpr uint32_t kStepMs = 10;
  static constexpr uint32_t kTimeoutMs = 120000;
  static constexpr uint32_t kElapsedSlackMs = 50;

  // Runs the script to its end, or to the first failure. Prints every listed difference.
  bool run(const std::vector<WireLine>& script) {
    for (const WireLine& w : script) {
      if (w.directive != "initial" && !w.topic.empty() && full(w.topic).empty())
        return failAt(w, "no channel for " + w.topic);
      if (w.directive == "expect" || w.directive == "extra" || w.directive == "differs") watched_.insert(full(w.topic));
    }
    for (const WireLine& w : script) {
      if (!line(w)) return false;
    }
    advance(1000);
    return noneUnmatched(script.back());
  }

  uint32_t now() const { return now_; }
  int listedDifferences() const { return differences_; }

private:
  mqtt::InMemoryMockBroker& broker_;
  mqtt::IMqttClient& office_;
  WireChannels channels_;
  std::function<void(uint32_t)> tick_;
  uint32_t now_;
  uint32_t time_locking_ms_;
  std::set<std::string> watched_;
  size_t begin_ = 0;         // log index where checking starts
  size_t last_matched_ = 0;  // log index of the last matched message (+1)
  std::vector<size_t> consumed_;
  std::vector<std::pair<std::string, uint32_t>> marks_;
  std::vector<uint32_t> arrival_;  // tick time at which each log entry was seen
  uint32_t last_time_ = 0;
  std::string step_;
  int differences_ = 0;

  std::string full(const std::string& topic) const { return wireTopic(channels_, topic); }
  bool watched(const std::string& topic) const { return watched_.count(topic) || alwaysWatched(channels_, topic); }

  void advance(uint32_t ms) {
    const uint32_t end = now_ + ms;
    while (static_cast<int32_t>(end - now_) > 0) stepOnce();
  }
  void stepOnce() {
    now_ += kStepMs;
    tick_(now_);
    stamp();
  }
  void stamp() {
    while (arrival_.size() < broker_.log().size()) arrival_.push_back(now_);
  }
  bool isConsumed(size_t i) const {
    for (size_t c : consumed_)
      if (c == i) return true;
    return false;
  }
  // First unconsumed message on topic at or after begin_, or SIZE_MAX.
  size_t next(const std::string& topic) const {
    for (size_t i = begin_; i < broker_.log().size(); ++i)
      if (broker_.log()[i].topic == topic && !isConsumed(i)) return i;
    return SIZE_MAX;
  }

  bool failAt(const WireLine& w, const std::string& what) {
    std::printf("    script line %d (%s): %s\n", w.number, step_.c_str(), what.c_str());
    return false;
  }

  bool line(const WireLine& w) {
    stamp();
    const std::string topic = full(w.topic);
    if (w.directive == "initial") return true;  // applied by whoever built the station
    if (w.directive == "step") {
      step_ = w.payload;
      return true;
    }
    if (w.directive == "begin") {
      begin_ = broker_.log().size();
      last_matched_ = begin_;
      return true;
    }
    if (w.directive == "send") {
      const size_t sent = broker_.log().size();  // the broker logs before it delivers
      office_.publish(topic, w.payload, false);
      stamp();
      last_matched_ = sent + 1;  // what follows answers this
      return true;
    }
    if (w.directive == "retained") {
      const std::string* r = broker_.retained(topic);
      if (r == nullptr || *r != w.payload)
        return failAt(w, "retained " + w.topic + " is \"" + (r ? *r : std::string("<none>")) + "\", script \"" +
                             w.payload + "\"");
      return true;
    }
    if (w.directive == "await") {
      for (uint32_t waited = 0;; waited += kStepMs) {
        for (size_t i = next(topic); i != SIZE_MAX; i = next(topic)) {
          consumed_.push_back(i);
          if (broker_.log()[i].payload == w.payload) return true;
        }
        if (waited >= kTimeoutMs) return failAt(w, "never saw " + w.payload);
        stepOnce();
      }
    }
    if (w.directive == "quiet") {
      uint32_t ms = 0;
      if (!parseUint(w.payload, 600000, ms)) return failAt(w, "quiet needs milliseconds");
      advance(ms);
      return noneUnmatched(w);
    }
    if (w.directive == "mark") {
      marks_.emplace_back(w.payload, last_time_);
      return true;
    }
    if (w.directive == "elapsed") {
      for (const auto& m : marks_) {
        if (m.first != w.payload) continue;
        const uint32_t e = last_time_ - m.second;
        if (e < time_locking_ms_ || e > time_locking_ms_ + kElapsedSlackMs)
          return failAt(w, "elapsed since " + w.payload + " is " + std::to_string(e) + " ms, time locking " +
                               std::to_string(time_locking_ms_) + " ms");
        return true;
      }
      return failAt(w, "no mark " + w.payload);
    }
    // expect / extra / differs: the next message on topic.
    for (uint32_t waited = 0;; waited += kStepMs) {
      const size_t i = next(topic);
      if (i != SIZE_MAX) {
        const mqtt::InMemoryMockBroker::Message& m = broker_.log()[i];
        consumed_.push_back(i);
        if (m.payload != w.payload)
          return failAt(w, "on " + w.topic + "\n        observed \"" + m.payload + "\"\n        script   \"" +
                               w.payload + "\"");
        if (i < last_matched_)
          return failAt(w, "\"" + w.payload + "\" on " + w.topic + " arrived before the message of the line above");
        last_matched_ = i + 1;
        last_time_ = arrival_[i];
        if (w.directive == "differs") {
          ++differences_;
          std::printf("    LISTED DIFFERENCE line %d (%s):\n        observed    %s %s\n        Walkthrough %s\n",
                      w.number, step_.c_str(), w.topic.c_str(), w.payload.c_str(), w.walkthrough.c_str());
        }
        return true;
      }
      if (waited >= kTimeoutMs) return failAt(w, "nothing on " + w.topic + " within " + std::to_string(kTimeoutMs) + " ms");
      stepOnce();
    }
  }

  bool noneUnmatched(const WireLine& w) {
    stamp();
    bool ok = true;
    for (size_t i = begin_; i < broker_.log().size(); ++i) {
      const mqtt::InMemoryMockBroker::Message& m = broker_.log()[i];
      if (watched(m.topic) && !isConsumed(i)) {
        ok = failAt(w, "unscripted " + m.topic + " \"" + m.payload + "\"");
        consumed_.push_back(i);
      }
    }
    return ok;
  }
};

}  // namespace codeline::testing
