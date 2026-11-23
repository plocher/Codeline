// The field side of the Code Line MQTT Binding (docs/bindings/CodeLineMQTT.md)
// for one station, over the engine's office API (codeline/engine/Office.h):
//
//   {station}/controls      subscribed, clean session; a delivery with the
//                           retain flag set is discarded unread (section 5
//                           rule 1)
//   {station}/indications   published retained after every control frame
//                           (accepted or not, rule 4), whenever any
//                           indication changes, and at start; the client's
//                           will publishes retained UNAVAILABLE here (rule 3)
//
// A control frame is read against the station's chart, taken as written from
// the station file (tools/station_file.py <- tools/codeline_chart.py):
//   - "CHART <id>" with another id: out of sync. Ignored as a whole.
//   - anything else not exactly the chart's terms in chart order, or two
//     asserted words of one positive-control group: malformed. Ignored as a
//     whole, vital and non-vital controls alike.
//   - valid: one ControlTransaction to the plant, which decides between
//     applied and valid-but-unsafe (README.CodeLine 5.3).
// Each outcome is counted, and told to onFrame's listener with a few words on
// why (the debug channel, DebugChannel.h; informative only). Controls are subscribed at QoS 0 (section 5 rule
// 1); the indications' QoS is the client's (MosquittoClient.h: QoS 1 for a
// retained publish).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codeline/engine/Plant.h"
#include "codeline/mqtt/IMqttClient.h"
#include "codeline/station/StationFile.h"

namespace codeline::station {

inline constexpr const char* kUnavailable = "UNAVAILABLE";

// {channel}/{name}, each space of the name replaced by '-' (CodeLineMQTT.md 1).
inline std::string stationTopic(std::string_view channel, std::string_view name) {
  std::string t(channel);
  t += '/';
  for (char c : name) t += c == ' ' ? '-' : c;
  return t;
}

class CodeLineField final : private mqtt::IMqttHandler {
public:
  enum class Outcome : uint8_t { Applied, VitalIgnored, Malformed, OutOfSync };

  struct Counts {
    uint32_t applied = 0, vital_ignored = 0, malformed = 0, out_of_sync = 0, retained_dropped = 0;
  };

  CodeLineField(mqtt::IMqttClient& client, const Chart& chart, engine::Plant& plant, std::string_view channel)
      : client_(client), chart_(chart), plant_(plant) {
    const std::string base = stationTopic(channel, chart.station);
    controls_ = base + "/controls";
    indications_ = base + "/indications";
  }

  const std::string& controlsTopic() const { return controls_; }
  const std::string& indicationsTopic() const { return indications_; }
  const Counts& counts() const { return counts_; }

  // Called after each control frame is decided, before the indications are
  // republished: the outcome, and why (empty when applied). Informative.
  void onFrame(std::function<void(Outcome, const std::string& why)> listener) { on_frame_ = std::move(listener); }

  // Every function of the chart exists in the plant: each control can be
  // asserted alone, each indication can be read. A runtime refuses to start otherwise.
  bool verify(std::string& error) const {
    for (const ChartFunction& f : chart_.controls) {
      engine::ControlTransaction t = plant_.newControls();
      t.assertFunction(f.cls, f.appliance, f.word);
      if (t.malformed()) return error = "chart control " + f.aar + " is no function of the plant", false;
    }
    const engine::Indications ind = plant_.indications();
    for (const ChartFunction& f : chart_.indications) {
      bool v = false;
      if (!ind.get(f.cls, f.appliance, f.word, v))
        return error = "chart indication " + f.aar + " is no function of the plant", false;
    }
    return true;
  }

  // Connects with the will and subscribes. The first frame goes out at the first tick().
  void connect(std::string_view client_id) {
    const mqtt::MqttWill will{indications_, kUnavailable, true};
    client_.connect(client_id, &will);
    client_.subscribeAtQos(controls_, this, 0);  // CodeLineMQTT.md 5 rule 1
  }

  // After each plant tick: publish when any indication changed.
  void tick() {
    const std::string frame = indicationFrame();
    if (frame != last_) publish(frame);
  }

  // Clean shutdown: the station's own UNAVAILABLE, then a disconnect (no will).
  void shutdown() {
    if (!client_.connected()) return;
    client_.publish(indications_, kUnavailable, true);
    client_.disconnect();
  }

  std::string indicationFrame() const {
    const engine::Indications ind = plant_.indications();
    std::string out = "CHART " + chart_.id;
    for (const ChartFunction& f : chart_.indications) {
      bool v = false;
      ind.get(f.cls, f.appliance, f.word, v);  // verify() proved it exists
      out += v ? " " + f.aar : " (" + f.aar + ")";
    }
    return out;
  }

  // One control frame, as received (exposed for scenarios; onMessage calls it).
  Outcome receive(std::string_view payload) {
    why_.clear();
    const Outcome o = decide(payload);
    if (on_frame_) on_frame_(o, why_);
    switch (o) {
      case Outcome::Applied: ++counts_.applied; break;
      case Outcome::VitalIgnored: ++counts_.vital_ignored; break;
      case Outcome::Malformed: ++counts_.malformed; break;
      case Outcome::OutOfSync: ++counts_.out_of_sync; break;
    }
    publish(indicationFrame());  // after every control frame (CodeLineMQTT.md 5 rule 4)
    return o;
  }

private:
  mqtt::IMqttClient& client_;
  const Chart& chart_;
  engine::Plant& plant_;
  std::string controls_, indications_;
  std::string last_;
  Counts counts_;
  std::function<void(Outcome, const std::string&)> on_frame_;
  std::string why_;  // why the frame being decided is not applied, for on_frame_

  Outcome malformed(std::string why) {
    why_ = std::move(why);
    return Outcome::Malformed;
  }

  void publish(const std::string& frame) {
    client_.publish(indications_, frame, true);
    last_ = frame;
  }

  void onMessage(std::string_view topic, std::string_view payload, bool retained) override {
    if (topic != controls_) return;
    if (retained) {  // never meant to be retained: discarded unread (CodeLineMQTT.md 5 rule 1)
      ++counts_.retained_dropped;
      return;
    }
    receive(payload);
  }

  static std::vector<std::string_view> terms(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
      size_t j = i;
      while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\r' && s[j] != '\n') ++j;
      if (j > i) out.push_back(s.substr(i, j - i));
      i = j;
    }
    return out;
  }

  Outcome decide(std::string_view payload) {
    const std::vector<std::string_view> t = terms(payload);
    if (t.size() < 2 || t[0] != "CHART") return malformed("it does not open with CHART and a chart identity");
    if (t[1] != chart_.id) {
      why_ = "the frame is for chart " + std::string(t[1]) + ", this field's chart is " + chart_.id;
      return Outcome::OutOfSync;
    }
    if (t.size() - 2 != chart_.controls.size())
      return malformed(std::to_string(t.size() - 2) + " terms, the chart has " + std::to_string(chart_.controls.size()) +
                       " controls");
    std::vector<int> asserted_in_group;
    engine::ControlTransaction tx = plant_.newControls();
    for (size_t i = 0; i < chart_.controls.size(); ++i) {
      const ChartFunction& f = chart_.controls[i];
      std::string_view term = t[i + 2];
      const bool dropped = term.size() >= 2 && term.front() == '(' && term.back() == ')';
      if (dropped) term = term.substr(1, term.size() - 2);
      if (term != f.aar)  // missing, extra, unknown or out of order
        return malformed("term " + std::to_string(i + 1) + " is " + std::string(t[i + 2]) + ", the chart has " + f.aar +
                         " there");
      if (dropped) continue;
      if (f.group != 0) {
        for (int g : asserted_in_group)
          if (g == f.group) return malformed(f.aar + " is a second asserted word of its positive-control group");
        asserted_in_group.push_back(f.group);
      }
      tx.assertFunction(f.cls, f.appliance, f.word);
    }
    if (tx.malformed()) return malformed("it names a function the plant does not have");
    switch (plant_.apply(tx)) {
      case engine::ControlResult::Applied: return Outcome::Applied;
      case engine::ControlResult::VitalIgnored:
        why_ = plant_.lastRefusal();
        return Outcome::VitalIgnored;
      case engine::ControlResult::Malformed: break;
    }
    return malformed("the plant does not take it");
  }
};

}  // namespace codeline::station
