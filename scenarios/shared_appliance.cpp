// Two stations on one broker that both read the same appliance: Christopher
// and Sargent each list track circuit 01T (sensor/01T on the appliance
// channel). It is one appliance on the wire, so one host simulates it and the
// other is told it is remote (HostConfig::remote, StationHost.h; the real
// host's --remote, passed by tools/run_layout.py): one /state writer, both
// plants read it, and when its host stops the other plant reads it unhealthy.
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
  int appliance_clients_for_01t = 0;
  bool ok = false;

  Station(mqtt::InMemoryMockBroker& broker, const char* file, bool remote) {
    std::string error;
    ok = station::loadStationFile(file, spec, error);
    station::HostConfig config;
    config.time_locking_ms = 3000;
    if (remote) config.remote.push_back("sensor/01T");
    host = std::make_unique<station::StationHost>(spec, config, [this, &broker](const std::string& id) {
      appliance_clients_for_01t += id == spec.appliance_channel + "/sensor/01T";
      return std::make_unique<mqtt::InMemoryMockBroker::Client>(broker);
    });
    ok = ok && host->start(error);
    if (!ok) std::printf("    %s\n", error.c_str());
  }
  const engine::TrackCircuit& circuit() { return *host->plant().findTrackCircuit("01T"); }
};

}  // namespace

SCENARIO("a track circuit two stations share is simulated by one host and read by both plants") {
  mqtt::InMemoryMockBroker broker;
  Station christopher(broker, "build/Christopher.station", false);
  Station sargent(broker, "build/Sargent.station", true);
  CHECK(christopher.ok && sargent.ok);
  CHECK(christopher.appliance_clients_for_01t == 1);
  CHECK(sargent.appliance_clients_for_01t == 0);  // no second writer of sensor/01T/state
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
  world.publish(christopher.spec.simulation_channel + "/sensor/01T", "SHUNT", false);
  run(500);
  CHECK(christopher.circuit().occupied());
  CHECK(sargent.circuit().occupied());
  const std::string plant = station::stationTopic(sargent.spec.codeline_channel, "Sargent") + "/debug/plant";
  CHECK(broker.retained(plant)->find("circuit 01T occupied\n") != std::string::npos);
  // The remote host has no driver for it: a simulation payload or a power cut addressed to it
  // there is refused, and changes nothing (the host that simulates it heard the same topic).
  CHECK(!sargent.host->simulate("sensor/01T", "(SHUNT)"));
  CHECK(!sargent.host->simulate("sensor/01T", "(POWER)"));
  run(500);
  CHECK(christopher.circuit().occupied() && sargent.circuit().occupied());
  const std::string state = christopher.spec.appliance_channel + "/sensor/01T/state";
  const std::string live = *broker.retained(state);
  CHECK(live != "UNAVAILABLE");
  christopher.host->shutdown();  // the host that simulates it stops
  for (int i = 0; i < 100; ++i) sargent.host->tick(t += 10);
  CHECK(!sargent.circuit().healthy() && !sargent.circuit().clear());  // read occupied, never trusted
  CHECK(broker.retained(plant)->find("circuit 01T unhealthy (UNAVAILABLE)\n") != std::string::npos);
}

SCENARIO("the remote host stopping leaves the shared appliance alone: its /state stays live and its own host reads it on") {
  mqtt::InMemoryMockBroker broker;
  Station christopher(broker, "build/Christopher.station", false);
  Station sargent(broker, "build/Sargent.station", true);
  uint32_t t = 1000;
  for (int i = 0; i < 500; ++i) {
    t += 10;
    christopher.host->tick(t);
    sargent.host->tick(t);
  }
  const std::string state = christopher.spec.appliance_channel + "/sensor/01T/state";
  const std::string live = *broker.retained(state);
  CHECK(live != "UNAVAILABLE");
  sargent.host->shutdown();
  for (int i = 0; i < 300; ++i) christopher.host->tick(t += 10);
  CHECK(*broker.retained(state) == live);  // Sargent said goodbye for its own appliances only
  CHECK(*broker.retained(sargent.spec.appliance_channel + "/sensor/835T1/state") == "UNAVAILABLE");
  CHECK(christopher.circuit().healthy() && christopher.circuit().clear());
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
  CHECK(refused("sensor/829T1", nullptr) == "remote sensor/829T1 is no appliance of station Sargent");
  CHECK(refused("sensor/01T", "sensor/01T") ==
        "initial sensor/01T SHUNT: sensor/01T is remote (another host simulates it; give that host the initial)");
}

int main() { return codeline::scenario::run(); }
