// Field lamps of the chart that are not levers' own lamps.
//
//   Sargent HBA   a contact (hot box alarm) on a sensor: HBAK is lit unless the
//                 detector is healthy and proves it open.
//   Luchessa EFL, WFL   chart lamps the project file gives no source for: the
//                 station starts and they read dark (a stopgap of the station file).
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/station/StationFile.h"
#include "codeline/station/StationHost.h"
#include "scenario.h"

using namespace codeline;

namespace {

struct Station {
  mqtt::InMemoryMockBroker broker;
  station::StationSpec spec;
  std::string indications_topic;
  std::unique_ptr<station::StationHost> host;
  mqtt::InMemoryMockBroker::Client world{broker};
  uint32_t t = 1000;
  bool ok = false;

  explicit Station(const char* file) {
    std::string error;
    ok = station::loadStationFile(file, spec, error);
    if (!ok) std::printf("    %s\n", error.c_str());
    indications_topic = station::stationTopic(spec.codeline_channel, spec.chart.station) + "/indications";
    station::HostConfig config;
    config.time_locking_ms = 3000;
    host = std::make_unique<station::StationHost>(spec, config, [this](const std::string&) {
      return std::make_unique<mqtt::InMemoryMockBroker::Client>(broker);
    });
    world.connect("world", nullptr);
    if (!ok) return;
    ok = host->start(error);
    if (!ok) std::printf("    %s\n", error.c_str());
    run(5000);
  }
  void run(uint32_t ms) {
    for (const uint32_t end = t + ms; t < end;) host->tick(t += 10);
  }
  void simulate(const char* appliance, const char* payload) {
    world.publish(spec.simulation_channel + "/" + appliance, payload, false);
    run(1000);
  }
  // True if the retained indication frame carries exactly this term.
  bool shows(const std::string& term) const {
    const std::string* r = broker.retained(indications_topic);
    std::istringstream in(r ? *r : "");
    for (std::string w; in >> w;)
      if (w == term) return true;
    return false;
  }
};

}  // namespace

SCENARIO("Sargent HBA: the alarm lamp follows the contact, and is lit whenever the detector cannot be read") {
  Station s("build/Sargent.station");
  CHECK(s.ok);
  CHECK(s.shows("(HBAK)"));  // detector healthy and quiet
  s.simulate("sensor/HBA", "SHUNT");
  CHECK(s.shows("HBAK"));
  s.simulate("sensor/HBA", "(SHUNT)");
  s.run(1500);  // the sensor's dropout hold-off (Cluster:HOLDOFF, 1500 ms)
  CHECK(s.shows("(HBAK)"));
  s.simulate("sensor/HBA", "(POWER)");  // the detector dies: not known quiet, so the alarm shows
  CHECK(s.shows("HBAK"));
  s.simulate("sensor/HBA", "POWER");
  s.run(3000);
  CHECK(s.shows("(HBAK)"));
}

SCENARIO("Luchessa EFL, WFL: lamps with no source in the project file let the station start and read dark") {
  Station s("build/Luchessa.station");
  CHECK(s.ok);
  CHECK(s.shows("(EFLK)"));
  CHECK(s.shows("(WFLK)"));
}

int main() { return codeline::scenario::run(); }
