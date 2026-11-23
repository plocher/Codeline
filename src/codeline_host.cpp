// The station host on a real MQTT broker: one station, built from its station
// file, with real cores over simulated drivers behind their MQTT adapters, the
// engine on the MQTT proxies, and the code line binding (StationHost.h), every
// client a libmosquitto connection (MosquittoClient.h). Plant time (what the
// station's tick gets) is the wall clock passed through a SimClock (SimClock.h):
// PAUSE, RUN, STEP <ms> and RATE <permille> on {simulation}/clock stop, step
// and scale it, and the host states it retained on {simulation}/clock/{station}.
// While plant time stands still the station is not ticked at all (nothing in
// the plant moves; a control frame is still answered from the last scan), and
// the MQTT connections are served as always.
//
// The debug and clock connections are informative: losing either never stops
// the host; each connects again by itself (DebugChannel.h, SimClock.h).
//
//   build/codeline_host --station build/Corporal.station [--broker HOST:PORT]
//                       [--time-locking-ms N] [--initial ROLE/ID=PAYLOAD ...]
//                       [--simulation-channel TOPIC] [--remote ROLE/ID ...]
//
// --time-locking-ms overrides the station file's time locking (a stopgap, see
// tools/station_file.py) so a scenario need not wait 30 s. --initial applies a
// simulation payload before power-on (--initial turnout/829=REVERSE).
// --simulation-channel overrides the station file's simulation channel.
// --remote names an appliance another host of the layout simulates (a track
// circuit two stations share): this host binds to it and simulates nothing
// for it (tools/run_layout.py passes it).
//
// Prints "ready" on stdout once the plant has made its first scan and every
// client is connected. Stops on SIGINT or SIGTERM (exit 0), and exits 3 if a
// connection the station needs breaks (any but debug and clock). However it
// stops, no retained topic of this host is left reading live: every client
// still connected says UNAVAILABLE itself on its retained topic (the
// indications, debug/plant, the clock, every appliance's /state) before it
// disconnects, and a client that lost its connection had its will published.
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
#include "codeline/station/SimClock.h"
#include "codeline/station/StationHost.h"

using namespace codeline;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

int usage() {
  std::fprintf(stderr,
               "usage: codeline_host --station FILE [--broker HOST:PORT] [--time-locking-ms N]\n"
               "                     [--initial ROLE/ID=PAYLOAD ...] [--simulation-channel TOPIC]\n"
               "                     [--remote ROLE/ID ...]\n");
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
    } else if (a == "--remote") {
      config.remote.push_back(v);
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

  // Every connection is served (pump); losing a needed one stops the host.
  // The debug connection (and the clock's, below) is informative only.
  std::vector<mqtt::MosquittoClient*> clients, needed;
  const std::string debug_id = spec.chart.station + "-debug";
  station::StationHost station_host(spec, config, [&](const std::string& id) {
    auto c = std::make_unique<mqtt::MosquittoClient>(host, port);
    clients.push_back(c.get());
    if (id != debug_id) needed.push_back(c.get());
    return c;
  });
  const auto t0 = std::chrono::steady_clock::now();
  auto real_ms = [&t0]() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  };
  if (!station_host.start(error)) {  // nothing is connected yet
    std::fprintf(stderr, "codeline_host: %s\n", error.c_str());
    return 2;
  }
  mqtt::MosquittoClient clock_client(host, port);
  clients.push_back(&clock_client);
  station::ClockChannel clock(clock_client,
                              config.simulation_channel.empty() ? spec.simulation_channel : config.simulation_channel,
                              spec.chart.station, 1000);  // plant time starts at 1000 ms
  // The one way out once anything is connected, whatever the reason.
  auto stop = [&](int rc) {
    station_host.shutdown();
    clock.shutdown();
    for (mqtt::MosquittoClient* c : clients) c->pump();
    const auto& n = station_host.field().counts();
    std::fprintf(stderr,
                 "codeline_host: stopped; control frames applied %u, vital ignored %u, malformed %u, out of sync %u, "
                 "retained dropped %u\n",
                 n.applied, n.vital_ignored, n.malformed, n.out_of_sync, n.retained_dropped);
    return rc;
  };
  clock.connect(spec.chart.station + "-clock");
  uint32_t ticked_ms = clock.now(real_ms());
  station_host.tick(ticked_ms);  // the plant's first scan; the code line field connects in it
  for (mqtt::MosquittoClient* c : clients) {
    if (!c->connected()) {
      std::fprintf(stderr, "codeline_host: no connection to the broker at %s:%d\n", host.c_str(), port);
      return stop(3);
    }
  }
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::printf("ready station %s chart %s broker %s:%d\n", spec.chart.station.c_str(), spec.chart.id.c_str(),
              host.c_str(), port);
  std::fflush(stdout);

  std::string said_mode = clock.clock().mode();
  while (!g_stop) {
    for (mqtt::MosquittoClient* c : clients) c->pump();  // a clock command arrives here
    const uint32_t now_ms = clock.now(real_ms());
    if (clock.clock().mode() != said_mode) station_host.debug().say("clock " + (said_mode = clock.clock().mode()));
    if (now_ms != ticked_ms) station_host.tick(ticked_ms = now_ms);  // plant time stands still: no scan
    for (mqtt::MosquittoClient* c : clients) c->pump();
    for (mqtt::MosquittoClient* c : needed) {
      if (!c->lost()) continue;
      std::fprintf(stderr, "codeline_host: lost the broker connection of %s\n", c->id().c_str());
      return stop(3);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return stop(0);
}
