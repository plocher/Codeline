// Plant time a person can pause, step and slow down (src/codeline/station/
// SimClock.h), on the simulation channel over InMemoryMockBroker:
//
//   {simulation}/clock             PAUSE | RUN | STEP <ms> | RATE <permille>
//   {simulation}/clock/{station}   RUNNING|PAUSED RATE <permille> T <plant ms>, retained
//
// A step runs at the clock's rate in real time and then pauses. Real time is injected (the real host passes its wall clock), so the scenario
// says exactly how much real time passed and reads how much plant time did.
// The last scenario runs CP Corporal on that plant time: its time locking
// stands still while paused and runs out by STEP alone.
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codeline/mqtt/InMemoryMockBroker.h"
#include "codeline/station/SimClock.h"
#include "codeline/station/StationHost.h"
#include "scenario.h"

using namespace codeline;

namespace {

const char* kSim = "/layout/SPCoast/simulation";

struct Rig {
  mqtt::InMemoryMockBroker broker;
  mqtt::InMemoryMockBroker::Client client{broker}, person{broker};
  uint32_t real;  // any origin
  station::ClockChannel clock;
  uint32_t plant = 0;

  explicit Rig(uint32_t real_start = 50000, uint32_t plant_start = 1000)
      : real(real_start), clock(client, kSim, "Christopher North", plant_start) {
    person.connect("person", nullptr);
    clock.connect("Christopher-North-clock");
    plant = clock.now(real);
  }
  // Real time passes, the host's loop turning every 5 ms.
  void pass(uint32_t ms) {
    for (const uint32_t end = real + ms; real != end;) plant = clock.now(real += 5);
  }
  void say(const char* command, bool retained = false) { person.publish(clock.commandsTopic(), command, retained); }
  std::string state() const {
    const std::string* r = broker.retained(clock.stateTopic());
    return r ? *r : "";
  }
  std::string at(const char* mode, uint32_t t) const { return std::string(mode) + " T " + std::to_string(t); }
  size_t published() const {
    size_t n = 0;
    for (const auto& m : broker.log()) n += m.topic == clock.stateTopic();
    return n;
  }
};

// Passes everything on, and remembers the QoS each subscription asked for.
struct QosRecorder final : mqtt::IMqttClient {
  mqtt::InMemoryMockBroker::Client inner;
  std::vector<std::pair<std::string, int>> subscriptions;
  explicit QosRecorder(mqtt::InMemoryMockBroker& b) : inner(b) {}
  void connect(std::string_view id, const mqtt::MqttWill* will) override { inner.connect(id, will); }
  void disconnect() override { inner.disconnect(); }
  bool connected() const override { return inner.connected(); }
  void publish(std::string_view t, std::string_view p, bool r) override { inner.publish(t, p, r); }
  void subscribe(std::string_view t, mqtt::IMqttHandler* h) override {
    subscriptions.emplace_back(std::string(t), -1);  // no QoS stated
    inner.subscribe(t, h);
  }
  void subscribeAtQos(std::string_view t, mqtt::IMqttHandler* h, uint8_t qos) override {
    subscriptions.emplace_back(std::string(t), qos);
    inner.subscribe(t, h);
  }
  void setLinkObserver(mqtt::IMqttLinkObserver* o) override { inner.setLinkObserver(o); }
};

}  // namespace

SCENARIO("running at real time: plant time follows real time, and the clock is stated retained about once a second") {
  Rig r;
  CHECK(r.clock.stateTopic() == "/layout/SPCoast/simulation/clock/Christopher-North");  // spelled as on the code line
  CHECK(r.plant == 1000);
  CHECK(r.state() == "RUNNING RATE 1000 T 1000");
  r.pass(3000);
  CHECK(r.plant == 4000);
  CHECK(r.state() == "RUNNING RATE 1000 T 4000");
  CHECK(r.published() == 4);  // at start, then once a second
}

SCENARIO("plant time starts where the host says, and real time may start anywhere and roll over") {
  Rig r(0xFFFFFE00u, 5000);  // real time is 512 ms short of 2^32
  CHECK(r.state() == "RUNNING RATE 1000 T 5000");
  r.pass(2000);  // across the rollover
  CHECK(r.real == 2000u - 512u);
  CHECK(r.plant == 7000);
  r.say("PAUSE");
  r.pass(1000);
  CHECK(r.state() == "PAUSED RATE 1000 T 7000");
}

SCENARIO("the commands are subscribed at QoS 0, like the code line's controls") {
  mqtt::InMemoryMockBroker broker;
  QosRecorder client(broker);
  station::ClockChannel clock(client, kSim, "Corporal", 1000);
  clock.connect("Corporal-clock");
  CHECK(client.subscriptions.size() == 1);
  CHECK(client.subscriptions[0].first == "/layout/SPCoast/simulation/clock" && client.subscriptions[0].second == 0);
}

SCENARIO("PAUSE stops plant time; RUN resumes from where it stood, with no jump for the real time that passed") {
  Rig r;
  r.pass(1000);
  r.say("PAUSE");
  r.pass(60000);
  CHECK(r.plant == 2000);
  CHECK(r.state() == "PAUSED RATE 1000 T 2000");
  const size_t n = r.published();
  r.pass(5000);
  CHECK(r.published() == n);  // nothing moves: nothing more is said
  r.say("RUN");
  r.pass(5);
  CHECK(r.plant == 2005);
  CHECK(r.state() == "RUNNING RATE 1000 T 2005");
  r.pass(995);
  CHECK(r.plant == 3000);
}

SCENARIO("STEP runs for exactly that much plant time at the current rate, in real time, then is paused again") {
  Rig r;
  r.say("PAUSE");
  r.pass(100);
  const uint32_t t0 = r.plant;
  r.say("STEP 250");
  r.pass(100);
  CHECK(r.plant == t0 + 100);  // not compressed: 100 ms of real time, 100 ms of the step
  CHECK(r.clock.clock().stepping() && !r.clock.clock().paused());
  CHECK(r.state() == r.at("RUNNING RATE 1000", t0 + 5));  // said when the step began to move
  r.pass(145);
  CHECK(r.plant == t0 + 245 && r.clock.clock().stepping());
  r.pass(5);
  CHECK(r.plant == t0 + 250 && !r.clock.clock().stepping() && r.clock.clock().paused());
  CHECK(r.state() == r.at("PAUSED RATE 1000", t0 + 250));  // said when the step ended
  r.pass(10000);
  CHECK(r.plant == t0 + 250);  // still paused
  r.say("RATE 100");
  r.say("STEP 20");  // at a tenth of real time: 200 ms
  r.pass(100);
  CHECK(r.plant == t0 + 260);
  r.pass(100);
  CHECK(r.plant == t0 + 270 && r.clock.clock().paused());
  r.pass(1000);
  CHECK(r.state() == r.at("PAUSED RATE 100", t0 + 270));
  r.say("STEP 3");  // shorter than one pass of the loop at real time: still said, still exact
  r.say("RATE 1000");
  r.pass(5);
  CHECK(r.plant == t0 + 273);
  CHECK(r.state() == r.at("PAUSED RATE 1000", t0 + 273));
}

SCENARIO("steps add up, to the limit of one hour and no further") {
  Rig r;
  r.say("PAUSE");
  r.pass(5);
  const uint32_t t0 = r.plant;
  r.say("STEP 7");
  r.say("STEP 3");
  r.pass(50);
  CHECK(r.plant == t0 + 10);
  r.say("RATE 10000");
  r.say("STEP 3599999");
  r.say("STEP 2");  // would pass the limit: refused whole
  r.say("STEP 1");  // exactly the limit
  r.say("STEP 1");  // refused
  r.pass(400000);   // more than the 360 s the hour takes at ten times real time
  CHECK(r.plant == t0 + 10 + 3600000);
  CHECK(r.clock.clock().paused());
}

SCENARIO("PAUSE during a step ends it there; RUN during a step goes on running") {
  Rig r;
  r.say("PAUSE");
  r.pass(5);
  const uint32_t t0 = r.plant;
  r.say("STEP 1000");
  r.pass(300);
  r.say("PAUSE");
  r.pass(5);
  CHECK(r.plant == t0 + 300 && !r.clock.clock().stepping());
  CHECK(r.state() == r.at("PAUSED RATE 1000", t0 + 300));
  r.pass(5000);
  CHECK(r.plant == t0 + 300);  // the other 700 ms were given up
  r.say("STEP 100");
  r.pass(50);
  r.say("RUN");
  r.pass(1000);
  CHECK(r.plant == t0 + 1350 && !r.clock.clock().stepping() && !r.clock.clock().paused());
  CHECK(r.state().rfind("RUNNING RATE 1000 T ", 0) == 0);  // RUNNING throughout: said once a second
}

SCENARIO("RATE scales plant time: 100 is ten times slower, 10000 ten times faster, and no fraction is lost") {
  Rig r;
  r.say("RATE 100");
  r.pass(5);
  CHECK(r.state() == "RUNNING RATE 100 T 1000");  // said at once; half a millisecond has passed
  r.pass(9995);
  CHECK(r.plant == 2000);  // 10 s real, 1 s plant
  r.say("RATE 10000");
  r.pass(1000);
  CHECK(r.plant == 12000);
  r.plant = r.clock.now(r.real += 1000000);  // the host stalled: 10^6 ms at once, times 10000, is past 32 bits
  CHECK(r.plant == 10012000);
  r.say("RATE 333");
  r.pass(3000);
  CHECK(r.plant == 10012999);  // 3000 * 0.333
  r.say("PAUSE");
  r.say("RATE 1000");  // the rate may change while paused
  r.pass(5);
  CHECK(r.state() == "PAUSED RATE 1000 T 10012999");
}

SCENARIO("what is not a command changes nothing: unknown words, values out of range, a STEP while running, a retained command") {
  Rig r;
  for (const char* bad : {"", "pause", "PAUSE ", "STOP", "STEP 100", "STEP", "RATE", "RATE 0", "RATE 10001", "RATE -5", "RATE 1e3",
                          "RATE  100"})
    r.say(bad);
  r.say("PAUSE", true);  // retained: a command is never replayed
  r.pass(1000);
  CHECK(r.plant == 2000);
  CHECK(r.state() == "RUNNING RATE 1000 T 2000");
  r.say("PAUSE");
  for (const char* bad : {"STEP 0", "STEP 3600001", "STEP x", "STEP 10 20"}) r.say(bad);
  r.pass(1000);
  CHECK(r.plant == 2000);
  CHECK(!r.clock.clock().stepping());
  r.person.publish(r.clock.commandsTopic(), "", true);
}

SCENARIO("the clock's connection is lost: plant time runs on, the will says UNAVAILABLE, and the clock connects and states itself again") {
  Rig r;
  r.broker.dropConnection(r.client);
  CHECK(r.state() == "UNAVAILABLE");
  r.say("PAUSE");  // nobody hears it
  r.pass(900);
  CHECK(r.plant == 1900);  // the station is not held up
  CHECK(r.state() == "UNAVAILABLE");
  r.pass(1100);  // the first retry is due 1 to 1.25 s after the loss
  CHECK(r.client.connected());
  CHECK(r.plant == 3000);
  CHECK(r.state().rfind("RUNNING RATE 1000 T ", 0) == 0);
  r.say("PAUSE");  // subscribed again
  r.pass(100);
  CHECK(r.state() == "PAUSED RATE 1000 T 3000");
}

SCENARIO("a stop of the host leaves the clock retained UNAVAILABLE, and it does not connect again") {
  Rig q;
  q.clock.shutdown();
  CHECK(q.state() == "UNAVAILABLE");
  q.pass(30000);
  CHECK(!q.client.connected() && q.state() == "UNAVAILABLE");
}

SCENARIO("CP Corporal on a paused clock: time locking stands still however long the pause, and runs out by STEP alone") {
  mqtt::InMemoryMockBroker broker;
  station::StationSpec spec;
  std::string error;
  CHECK(station::loadStationFile("build/Corporal.station", spec, error));
  station::HostConfig config;
  config.time_locking_ms = 3000;
  station::StationHost host(spec, config,
                            [&broker](const std::string&) { return std::make_unique<mqtt::InMemoryMockBroker::Client>(broker); });
  mqtt::InMemoryMockBroker::Client clock_client(broker), person(broker);
  station::ClockChannel clock(clock_client, spec.simulation_channel, spec.chart.station, 1000);
  person.connect("person", nullptr);
  CHECK(host.start(error));
  clock.connect("Corporal-clock");
  // The real host's loop (src/codeline_host.cpp): the station is ticked only when plant time moved.
  uint32_t real = 0, ticked = clock.now(real);
  host.tick(ticked);
  auto pass = [&](uint32_t ms) {
    for (const uint32_t end = real + ms; real != end;) {
      const uint32_t now = clock.now(real += 5);
      if (now != ticked) host.tick(ticked = now);
    }
  };
  const std::string codeline = station::stationTopic(spec.codeline_channel, spec.chart.station);
  auto timing = [&] {  // 830TEK lit
    const std::string* f = broker.retained(codeline + "/indications");
    return f != nullptr && f->find(" 830TEK") != std::string::npos;
  };
  pass(5000);  // startup time locking expires
  CHECK(!timing());
  person.publish(spec.simulation_channel + "/sensor/Corporal:832T", "SHUNT", false);
  person.publish(codeline + "/controls", "CHART 85973c19 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)", false);
  pass(3000);
  person.publish(codeline + "/controls", "CHART 85973c19 (829NWS) (829RWS) (827WLS) (828WGS) (828EGS) 828HS (830WGS) (830EGS) 830HS (1MCS)", false);
  pass(1000);
  CHECK(timing());  // about 2.5 s of the 3 s are left
  person.publish(clock.commandsTopic(), "PAUSE", false);
  pass(60000);
  CHECK(timing());  // a minute of real time: not a millisecond of time locking
  // Paused, a control frame is still answered from the last scan, and the debug snapshot follows
  // it: the call is stored at once, but its lamp lights only when plant time moves.
  const size_t frames_before = broker.log().size();
  person.publish(codeline + "/controls", "CHART 85973c19 (829NWS) (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) 1MCS", false);
  size_t answers = 0;
  for (size_t i = frames_before; i < broker.log().size(); ++i) answers += broker.log()[i].topic == codeline + "/indications";
  CHECK(answers == 1);
  CHECK(broker.retained(codeline + "/indications")->find(" (1MCK) ") != std::string::npos);
  CHECK(broker.retained(codeline + "/debug/plant")->find("call 1MC dark, called\n") != std::string::npos);
  pass(10000);  // still paused: nothing lights
  CHECK(broker.retained(codeline + "/indications")->find(" (1MCK) ") != std::string::npos);
  person.publish(clock.commandsTopic(), "STEP 2000", false);
  pass(2500);
  CHECK(broker.retained(codeline + "/indications")->find(" 1MCK ") != std::string::npos);
  CHECK(timing());
  CHECK(broker.retained(codeline + "/debug/plant")->find("lever 830 STOP, time locking 1 s remaining") != std::string::npos);
  person.publish(clock.commandsTopic(), "STEP 1000", false);
  pass(1500);
  CHECK(!timing());
  CHECK(*broker.retained(clock.stateTopic()) == "PAUSED RATE 1000 T " + std::to_string(ticked));
}

int main() { return codeline::scenario::run(); }
