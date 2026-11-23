// Appliance:TURNOUT scenarios that only exist on the MQTT path: retention,
// last will, exact wire frames, and the curated {channel}/all broadcast.
// Time moves only through rig.tick(now_ms).
#include <string>

#include "codeline/testing/TurnoutRig.h"
#include "scenario.h"

using namespace codeline;
using codeline::mqtt::InMemoryMockBroker;
using codeline::mqtt::MqttTurnoutProxy;
using codeline::testing::kTravelMs;
using codeline::testing::MqttRig;
using codeline::testing::Recorder;
using Points = SimulatedTurnoutDriver::Points;

static bool retainedIs(MqttRig& rig, const char* suffix, const char* payload) {
  const std::string* r = rig.broker().retained(rig.topic(suffix));
  return r && *r == payload;
}

// Whether the broker carried `payload` on `topic` with the given retain flag.
static bool sent(MqttRig& rig, const std::string& topic, const char* payload, bool retained) {
  for (const InMemoryMockBroker::Message& m : rig.broker().log()) {
    if (m.topic == topic && m.payload == payload && m.retained == retained) return true;
  }
  return false;
}

SCENARIO("mqtt    boot publishes retained state and an unretained CONFIG event") {
  MqttRig rig;
  rig.tick(0);
  CHECK(sent(rig, rig.topic("state"), "NORMAL (REVERSE)", true));
  CHECK(sent(rig, rig.topic("event"),
             "CONFIG maintenance.post=false turnout.feedback_regime=TWO_CONTACT turnout.sim_transit_ms=1000",
             false));
  CHECK(rig.broker().retained(rig.topic("event")) == nullptr);
}

SCENARIO("mqtt    the office never retains a command, and state is retained") {
  MqttRig rig;
  rig.tick(0);
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  CHECK(sent(rig, rig.topic("cmd"), "(NORMAL) REVERSE", false));
  rig.tick(kTravelMs);
  CHECK(rig.broker().retained(rig.topic("cmd")) == nullptr);
  CHECK(retainedIs(rig, "state", "(NORMAL) REVERSE"));

  // A late subscriber learns the state from the broker alone.
  InMemoryMockBroker::Client late_client(rig.broker());
  MqttTurnoutProxy late(late_client, rig.adapter().applianceTopic());
  CHECK(late.state() == TurnoutState::InTransit);
  late.connect("late-office");
  CHECK(late.state() == TurnoutState::Reverse);
}

SCENARIO("mqtt    a retained /cmd, /maint or broadcast from a foreign publisher never acts, now or on reconnect") {
  MqttRig rig;
  Recorder rec;
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  rec.clear();
  InMemoryMockBroker::Client rogue(rig.broker());
  rogue.connect("rogue", nullptr);
  rogue.publish(rig.topic("cmd"), "(NORMAL) REVERSE", true);
  rogue.publish(rig.topic("maint"), "OFFLINE", true);
  rogue.publish(std::string(MqttRig::kChannel) + "/all", "IDENTIFY ON", true);
  CHECK(rig.broker().retained(rig.topic("cmd")) != nullptr);  // the broker did keep it
  rig.tick(10);
  CHECK(rig.driver().driveCount() == 0);
  CHECK(!rig.driver().beacon());
  CHECK(rec.events.empty());

  // The appliance reconnects and is handed all three as retained messages.
  rig.applianceClient().disconnect();
  rig.adapter().connect();
  rig.tick(kTravelMs * 2);
  CHECK(rig.driver().driveCount() == 0);
  CHECK(!rig.driver().beacon());
  CHECK(rec.count(EventKind::Offline) == 0);
  CHECK(rig.turnout().state() == TurnoutState::Normal);

  // A live, unretained command still works.
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  CHECK(rig.driver().driveCount() == 1);
}

SCENARIO("mqtt    the proxy losing its own link goes unproven, then recovers from retained state") {
  MqttRig rig;
  Recorder rec;
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  rec.clear();
  rig.broker().dropConnection(rig.officeClient());
  CHECK(rec.timeline == "IN_TRANSIT Unavailable ");
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
  CHECK(retainedIs(rig, "state", "NORMAL (REVERSE)"));  // the appliance is fine
  rig.proxy().connect("office");
  CHECK(rig.turnout().state() == TurnoutState::Normal);
}

SCENARIO("mqtt    an empty, malformed or contradictory /state makes the proxy unproven") {
  MqttRig rig;
  rig.tick(0);
  InMemoryMockBroker::Client rogue(rig.broker());
  rogue.connect("rogue", nullptr);
  const char* bad[] = {"NORMAL REVERSE", "", "SIDEWAYS", "NORMAL (REVERSE) EXTRA"};
  for (const char* payload : bad) {
    rogue.publish(rig.topic("state"), "NORMAL (REVERSE)", false);
    CHECK(rig.turnout().state() == TurnoutState::Normal);
    rogue.publish(rig.topic("state"), payload, false);
    CHECK(rig.turnout().state() == TurnoutState::InTransit);
  }
}

SCENARIO("mqtt    losing the appliance fires its will: UNAVAILABLE, then recovery") {
  MqttRig rig;
  Recorder rec;
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  rec.clear();
  rig.broker().dropConnection(rig.applianceClient());
  CHECK(rec.timeline == "IN_TRANSIT Unavailable ");
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
  CHECK(retainedIs(rig, "state", "UNAVAILABLE"));

  // Commands while it is gone go nowhere and are not kept.
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  rig.tick(kTravelMs);
  CHECK(rig.driver().driveCount() == 0);

  rec.clear();
  rig.adapter().connect();
  CHECK(rec.timeline == "NORMAL ");
  CHECK(retainedIs(rig, "state", "NORMAL (REVERSE)"));
}

SCENARIO("mqtt    a clean disconnect does not fire the will") {
  MqttRig rig;
  Recorder rec;
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  rec.clear();
  rig.applianceClient().disconnect();
  CHECK(rec.events.empty());
  CHECK(retainedIs(rig, "state", "NORMAL (REVERSE)"));
}

SCENARIO("mqtt    shorted contacts put (NORMAL) (REVERSE) on /state and FAULT on /event") {
  MqttRig rig;
  rig.tick(0);
  rig.driver().shortContacts();
  rig.tick(10);
  CHECK(retainedIs(rig, "state", "(NORMAL) (REVERSE)"));
  CHECK(sent(rig, rig.topic("event"), "FAULT SHORTED_CONTACTS (both point contacts closed)", false));
}

SCENARIO("mqtt    /maint frames: SET with or without spaces, malformed frames dropped") {
  MqttRig rig;
  Recorder rec;
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  InMemoryMockBroker::Client tool(rig.broker());
  tool.connect("bench-tool", nullptr);
  const std::string maint = rig.topic("maint");
  tool.publish(maint, "OFFLINE", false);
  tool.publish(maint, "SET turnout.sim_transit_ms+=250", false);
  CHECK(rec.lastValue(EventKind::Config, "turnout.sim_transit_ms") == 1250);
  tool.publish(maint, "SET turnout.feedback_regime = SIMULATED", false);
  CHECK(rec.lastValue(EventKind::Config, "turnout.feedback_regime") ==
        static_cast<uint32_t>(FeedbackRegime::Simulated));
  rec.clear();
  tool.publish(maint, "SET turnout.feedback_regime = BOTH", false);
  tool.publish(maint, "SET turnout.sim_transit_ms = -5", false);
  tool.publish(maint, "SET nonsense.path = 1", false);
  tool.publish(maint, "CYCLE 70000 10", false);
  tool.publish(maint, "EXPORT", false);
  tool.publish(maint, "OFFLINE now", false);
  CHECK(rec.events.empty());
  tool.publish(maint, "IMPORT turnout.sim_transit_ms=300 maintenance.post=true", false);
  CHECK(rec.lastValue(EventKind::Config, "turnout.sim_transit_ms") == 300);
  CHECK(rec.lastValue(EventKind::Config, "maintenance.post") == 1);
  tool.publish(maint, "CLEAR maintenance.fault_count", false);
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.fault_count") == 0);
}

SCENARIO("mqtt    {channel}/all: EXPORT CONFIG answers after jitter; lifecycle verbs and groups are dropped") {
  MqttRig rig;
  Recorder rec;
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  rec.clear();
  InMemoryMockBroker::Client studio(rig.broker());
  studio.connect("studio", nullptr);
  const std::string all = std::string(MqttRig::kChannel) + "/all";

  studio.publish(all, "OFFLINE", false);
  studio.publish(all, "SET turnout.sim_transit_ms = 5", false);
  studio.publish(all, "GROUP corporal EXPORT CONFIG", false);
  rig.tick(1000);
  CHECK(rec.events.empty());

  studio.publish(all, "EXPORT CONFIG", false);
  CHECK(rec.events.empty());  // not before the jitter has run
  rig.tick(1000 + mqtt::kBroadcastJitterMaxMs);
  CHECK(rec.count(EventKind::Config) == 1);

  studio.publish(all, "IDENTIFY 5000", false);
  CHECK(rig.driver().beacon());
  studio.publish(all, "IDENTIFY OFF", false);
  CHECK(!rig.driver().beacon());
}

SCENARIO("codec   TurnoutCommand frames: only the three valid pairs decode") {
  TurnoutCommand c;
  CHECK(mqtt::decodeCommand("NORMAL (REVERSE)", c) && c == TurnoutCommand::Normal);
  CHECK(mqtt::decodeCommand("(NORMAL) REVERSE", c) && c == TurnoutCommand::Reverse);
  CHECK(mqtt::decodeCommand("(NORMAL) (REVERSE)", c) && c == TurnoutCommand::Unchanged);
  CHECK(!mqtt::decodeCommand("NORMAL REVERSE", c));
  CHECK(!mqtt::decodeCommand("REVERSE (NORMAL)", c));
  CHECK(!mqtt::decodeCommand("NORMAL", c));
  CHECK(!mqtt::decodeCommand("NORMAL (REVERSE) EXTRA", c));
  CHECK(!mqtt::decodeCommand("(NORMAL (REVERSE)", c));
  for (uint8_t i = 0; i < kTurnoutCommandCount; ++i) {
    const auto cmd = static_cast<TurnoutCommand>(i);
    CHECK(mqtt::decodeCommand(mqtt::encodeCommand(cmd), c) && c == cmd);
  }
}

int main() { return codeline::scenario::run(); }
