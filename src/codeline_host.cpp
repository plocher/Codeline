// The station host on a real MQTT broker: one station, built from its station
// file, with real cores over simulated drivers behind their MQTT adapters, the
// engine on the MQTT proxies, and the code line binding (StationHost.h), every
// client a libmosquitto connection (MosquittoClient.h), time from the wall
// clock.
//
//   build/codeline_host --station build/Corporal.station [--broker HOST:PORT]
//                       [--time-locking-ms N] [--initial ROLE/ID=PAYLOAD ...]
//                       [--simulation-channel TOPIC]
//
// --time-locking-ms overrides the station file's time locking (a stopgap, see
// tools/station_file.py) so a scenario need not wait 30 s. --initial applies a
// simulation payload before power-on (--initial turnout/829=REVERSE).
// --simulation-channel overrides the station file's simulation channel.
//
// Prints "ready" on stdout once the plant has made its first scan and every
// client is connected. Stops cleanly on SIGINT or SIGTERM (the station's
// indications and every appliance's /state read UNAVAILABLE, published by the
// host itself), and exits 3 if any connection to the broker breaks (each will
// has then been published).
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "codeline/mqtt/MosquittoClient.h"
#include "codeline/station/StationHost.h"

using namespace codeline;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

int usage() {
  std::fprintf(stderr,
               "usage: codeline_host --station FILE [--broker HOST:PORT] [--time-locking-ms N]\n"
               "                     [--initial ROLE/ID=PAYLOAD ...] [--simulation-channel TOPIC]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string station_path, host = "127.0.0.1";
  int port = 1883;
  station::HostConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) return usage();
    const std::string v = argv[++i];
    if (a == "--station") {
      station_path = v;
    } else if (a == "--broker") {
      const size_t colon = v.rfind(':');
      if (colon == std::string::npos) return usage();
      host = v.substr(0, colon);
      port = std::atoi(v.c_str() + colon + 1);
    } else if (a == "--time-locking-ms") {
      config.time_locking_ms = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    } else if (a == "--initial") {
      const size_t eq = v.find('=');
      if (eq == std::string::npos) return usage();
      config.initial.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    } else if (a == "--simulation-channel") {
      config.simulation_channel = v;
    } else {
      return usage();
    }
  }
  if (station_path.empty() || port <= 0) return usage();

  station::StationSpec spec;
  std::string error;
  if (!station::loadStationFile(station_path.c_str(), spec, error)) {
    std::fprintf(stderr, "codeline_host: %s\n", error.c_str());
    return 2;
  }

  std::vector<mqtt::MosquittoClient*> clients;
  station::StationHost station_host(spec, config, [&](const std::string&) {
    auto c = std::make_unique<mqtt::MosquittoClient>(host, port);
    clients.push_back(c.get());
    return c;
  });
  const auto t0 = std::chrono::steady_clock::now();
  auto now_ms = [&t0]() {
    return 1000u + static_cast<uint32_t>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                           .count());
  };
  if (!station_host.start(error)) {
    std::fprintf(stderr, "codeline_host: %s\n", error.c_str());
    return 2;
  }
  station_host.tick(now_ms());  // the plant's first scan; the code line field connects in it
  for (mqtt::MosquittoClient* c : clients) {
    if (!c->connected()) {
      std::fprintf(stderr, "codeline_host: no connection to the broker at %s:%d\n", host.c_str(), port);
      return 3;
    }
  }
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::printf("ready station %s chart %s broker %s:%d\n", spec.chart.station.c_str(), spec.chart.id.c_str(),
              host.c_str(), port);
  std::fflush(stdout);

  int rc = 0;
  while (!g_stop) {
    for (mqtt::MosquittoClient* c : clients) c->pump();
    station_host.tick(now_ms());
    for (mqtt::MosquittoClient* c : clients) c->pump();
    bool lost = false;
    for (mqtt::MosquittoClient* c : clients) lost = lost || c->lost();
    if (lost) {
      std::fprintf(stderr, "codeline_host: lost the broker\n");
      rc = 3;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (rc == 0) {
    station_host.shutdown();
    for (mqtt::MosquittoClient* c : clients) c->pump();
  }
  const auto& n = station_host.field().counts();
  std::fprintf(stderr,
               "codeline_host: stopped; control frames applied %u, vital ignored %u, malformed %u, out of sync %u, "
               "retained dropped %u\n",
               n.applied, n.vital_ignored, n.malformed, n.out_of_sync, n.retained_dropped);
  return rc;
}
