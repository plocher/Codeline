// Two stations on one broker whose plants use the same name: Christopher and
// Sargent each have a track circuit 01T. On the appliance channel every
// appliance is fully qualified by its interlocking (sensor/Christopher:01T,
// sensor/Sargent:01T), so they are two appliances and neither host sees the
// other's.
//
// A host can also be told an appliance of its station is remote
// (HostConfig::remote, StationHost.h; the real host's --remote): something
// else publishes it, the host makes no driver and no client for it, and the
// plant reads it unhealthy until that something speaks.
#include <memory>
#include <string>

#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/station/StationHost.h"
#include "scenario.h"

using namespace codeline;

namespace {

struct Station {
  station::StationSpec spec;
  std::unique_ptr<station::StationHost> host;
  int clients_for_01t = 0;  // appliance-side clients for this station's own 01T
  bool ok = false;

  Station(mqtt::InMemoryMockBroker& broker, const char* file, const std::string& own_01t, bool remote) {
    std::string error;
    ok = station::loadStationFile(file, spec, error);
    station::HostConfig config;
    config.time_locking_ms = 3000;
    if (remote) config.remote.push_back(own_01t);
    host = std::make_unique<station::StationHost>(spec, config, [this, &broker, own_01t](const std::string& id) {
      clients_for_01t += id == spec.appliance_channel + "/" + own_01t;
      return std::make_unique<mqtt::InMemoryMockBroker::Client>(broker);
    });
    ok = ok && host->start(error);
    if (!ok) std::printf("    %s\n", error.c_str());
  }
  const engine::TrackCircuit& circuit() { return *host->plant().findTrackCircuit("01T"); }
};

const char* const kChristopher01T = "sensor/Christopher:01T";
const char* const kSargent01T = "sensor/Sargent:01T";

}  // namespace

SCENARIO("Christopher's 01T and Sargent's 01T are two track circuits: a shunt of one is not seen by the other") {
  mqtt::InMemoryMockBroker broker;
  Station christopher(broker, "build/Christopher.station", kChristopher01T, false);
  Station sargent(broker, "build/Sargent.station", kSargent01T, false);
  CHECK(christopher.ok && sargent.ok);
  CHECK(christopher.clients_for_01t == 1);
  CHECK(sargent.clients_for_01t == 1);
  mqtt::InMemoryMockBroker::Client world(broker);
  world.connect("world", nullptr);
  uint32_t t = 1000;
  auto run = [&](uint32_t ms) {
    for (const uint32_t end = t + ms; t < end;) {
      t += 10;
      christopher.host->tick(t);
      sargent.host->tick(t);
    }
  };
  run(5000);
  CHECK(christopher.circuit().healthy() && christopher.circuit().clear());
  CHECK(sargent.circuit().healthy() && sargent.circuit().clear());
  world.publish(christopher.spec.simulation_channel + "/" + kChristopher01T, "SHUNT", false);
  run(500);
  CHECK(christopher.circuit().occupied());
  CHECK(sargent.circuit().healthy() && sargent.circuit().clear());
  const std::string plant = station::stationTopic(sargent.spec.codeline_channel, "Sargent") + "/debug/plant";
  CHECK(broker.retained(plant)->find("circuit 01T clear\n") != std::string::npos);
  world.publish(sargent.spec.simulation_channel + "/" + kSargent01T, "SHUNT", false);
  world.publish(christopher.spec.simulation_channel + "/" + kChristopher01T, "(SHUNT)", false);
  run(3000);  // past the sensor's dropout hold-off
  CHECK(christopher.circuit().clear());
  CHECK(sargent.circuit().occupied());
}

SCENARIO("an appliance a host is told is remote gets no driver and no client there, reads unhealthy, and is left alone when the host stops") {
  mqtt::InMemoryMockBroker broker;
  Station sargent(broker, "build/Sargent.station", kSargent01T, true);
  CHECK(sargent.ok);
  CHECK(sargent.clients_for_01t == 0);  // no writer of its /state here
  uint32_t t = 1000;
  for (int i = 0; i < 500; ++i) sargent.host->tick(t += 10);
  CHECK(!sargent.circuit().healthy() && !sargent.circuit().clear());  // nobody speaks for it: read occupied, never trusted
  // The host has no driver for it: a simulation payload or a power cut addressed to it is refused.
  CHECK(!sargent.host->simulate(kSargent01T, "(SHUNT)"));
  CHECK(!sargent.host->simulate(kSargent01T, "(POWER)"));
  sargent.host->shutdown();
  CHECK(broker.retained(sargent.spec.appliance_channel + "/" + kSargent01T + "/state") == nullptr);  // goodbye for its own only
  CHECK(*broker.retained(sargent.spec.appliance_channel + "/sensor/Sargent:835T1/state") == "UNAVAILABLE");
}

SCENARIO("a remote appliance the station does not have, or an initial payload for a remote one, is refused before anything starts") {
  auto refused = [](const char* remote, const char* initial) {
    mqtt::InMemoryMockBroker broker;
    station::StationSpec spec;
    std::string error;
    station::loadStationFile("build/Sargent.station", spec, error);
    station::HostConfig config;
    config.remote.push_back(remote);
    if (initial) config.initial.emplace_back(initial, "SHUNT");
    station::StationHost host(spec, config,
                              [&broker](const std::string&) { return std::make_unique<mqtt::InMemoryMockBroker::Client>(broker); });
    const bool started = host.start(error);
    std::printf("    refused: %s\n", error.c_str());
    return started ? std::string("started") : error;
  };
  CHECK(refused("sensor/Christopher:01T", nullptr) == "remote sensor/Christopher:01T is no appliance of station Sargent");
  CHECK(refused(kSargent01T, kSargent01T) ==
        "initial sensor/Sargent:01T SHUNT: sensor/Sargent:01T is remote (another host simulates it; give that host the initial)");
}

int main() { return codeline::scenario::run(); }
