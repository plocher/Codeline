// Appliance:TURNOUT functional scenarios, written once against ITurnout and
// run twice: direct (TurnoutCore over SimulatedTurnoutDriver) and through the
// MQTT loopback (Proxy -> MockBroker -> Adapter -> TurnoutCore).
// Time moves only through rig.tick(now_ms).
#include "codeline/testing/TurnoutRig.h"
#include "scenario.h"

using namespace codeline;
using codeline::testing::DirectRig;
using codeline::testing::kTravelMs;
using codeline::testing::MqttRig;
using codeline::testing::Recorder;
using codeline::testing::TurnoutRig;
using Points = SimulatedTurnoutDriver::Points;

#define TURNOUT_SCENARIO(name)                                                                   \
  static void SCENARIO_CAT(turnout_body_, __LINE__)(TurnoutRig & rig);                           \
  static codeline::scenario::Registrar SCENARIO_CAT(turnout_direct_, __LINE__)(                   \
      "direct  " name, [] {                                                                      \
        DirectRig r;                                                                             \
        SCENARIO_CAT(turnout_body_, __LINE__)(r);                                                \
      });                                                                                        \
  static codeline::scenario::Registrar SCENARIO_CAT(turnout_mqtt_, __LINE__)(                     \
      "mqtt    " name, [] {                                                                      \
        MqttRig r;                                                                               \
        SCENARIO_CAT(turnout_body_, __LINE__)(r);                                                \
      });                                                                                        \
  static void SCENARIO_CAT(turnout_body_, __LINE__)(TurnoutRig & rig)

// Boots the rig at t=0 with `rec` observing, then forgets the boot traffic.
static void boot(TurnoutRig& rig, Recorder& rec) {
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  rec.clear();
}

TURNOUT_SCENARIO("startup reads the contacts without moving the points") {
  Recorder rec;
  rig.turnout().setObserver(&rec);
  CHECK(rig.turnout().state() == TurnoutState::InTransit);  // unproven before boot
  rig.tick(0);
  CHECK(rig.turnout().state() == TurnoutState::Normal);
  CHECK(rec.timeline == "NORMAL Config ");  // no OnlineEvent on power-on
  CHECK(rec.lastValue(EventKind::Config, "maintenance.post") == 0);
  CHECK(rig.driver().driveCount() == 0);
}

TURNOUT_SCENARIO("startup with the points reversed reports REVERSE") {
  Recorder rec;
  rig.driver().handThrow(Points::Reverse);
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  CHECK(rig.turnout().state() == TurnoutState::Reverse);
  CHECK(rig.driver().driveCount() == 0);
}

TURNOUT_SCENARIO("a throw passes through IN_TRANSIT to correspondence") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  CHECK(rig.turnout().state() == TurnoutState::Normal);  // motor energized, nothing read yet
  CHECK(rig.driver().motor() == TurnoutCommand::Reverse);
  rig.tick(10);
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
  rig.tick(kTravelMs - 1);
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
  rig.tick(kTravelMs);
  CHECK(rig.turnout().state() == TurnoutState::Reverse);
  CHECK(rec.timeline == "IN_TRANSIT REVERSE ");
}

TURNOUT_SCENARIO("UNCHANGED leaves the points alone") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().setAlignment(TurnoutCommand::Unchanged);
  rig.tick(kTravelMs);
  CHECK(rig.driver().driveCount() == 0);
  CHECK(rig.turnout().state() == TurnoutState::Normal);
}

TURNOUT_SCENARIO("a contradictory NORMAL REVERSE command is rejected at the codec") {
  Recorder rec;
  boot(rig, rec);
  rig.sendRawCommand("NORMAL REVERSE");
  rig.tick(kTravelMs);
  CHECK(rig.driver().driveCount() == 0);
  CHECK(rig.turnout().state() == TurnoutState::Normal);
  rig.sendRawCommand("(NORMAL) REVERSE");  // the same codec accepts a valid frame
  CHECK(rig.driver().motor() == TurnoutCommand::Reverse);
}

TURNOUT_SCENARIO("shorted contacts hold IN_TRANSIT and raise SHORTED_CONTACTS once") {
  Recorder rec;
  boot(rig, rec);
  rig.driver().shortContacts();
  rig.tick(10);
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
  CHECK(rec.timeline == "IN_TRANSIT Fault ");
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::ShortedContacts);
  rig.tick(20);
  CHECK(rec.count(EventKind::Fault) == 1);  // edge-triggered
  rig.turnout().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 1);
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.fault_count") == 1);

  rig.turnout().clearFault();  // ONLINE: ignored
  rig.driver().releaseContacts();
  rig.tick(30);
  CHECK(rig.turnout().state() == TurnoutState::Normal);
  rig.turnout().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 1);  // still latched
  rig.turnout().offline();
  rig.turnout().clearFault();  // contacts now verify
  rig.turnout().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 0);
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.fault_count") == 1);
}

TURNOUT_SCENARIO("a VITAL write while ONLINE is ignored") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().setAttribute("turnout.sim_transit_ms", SetOp::Assign, AttrValue::number(400));
  rig.turnout().setAttribute("turnout.feedback_regime", SetOp::Assign,
                             AttrValue::enumerator(FeedbackRegime::Simulated));
  CHECK(rec.events.empty());
  rig.turnout().exportConfig();
  CHECK(rec.lastValue(EventKind::Config, "turnout.sim_transit_ms") == 1000);
  CHECK(rec.lastValue(EventKind::Config, "turnout.feedback_regime") ==
        static_cast<uint32_t>(FeedbackRegime::TwoContact));
}

TURNOUT_SCENARIO("OFFLINE unlocks VITAL writes and ONLINE restarts with them") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().offline();
  rig.turnout().offline();  // no second event: offline was already true
  CHECK(rec.timeline == "Offline ");
  rig.turnout().setAttribute("turnout.sim_transit_ms", SetOp::Assign, AttrValue::number(400));
  CHECK(rec.lastValue(EventKind::Config, "turnout.sim_transit_ms") == 400);
  rig.turnout().setAttribute("turnout.feedback_regime", SetOp::Assign,
                             AttrValue::enumerator(FeedbackRegime::Simulated));
  rec.clear();

  rig.turnout().online();
  // RESTART: state, then CONFIG, then ONLINE last. SIMULATED has no contacts
  // to read, so the restarted turnout is unproven until commanded.
  CHECK(rec.timeline == "IN_TRANSIT Config Online ");
  CHECK(rec.lastValue(EventKind::Config, "turnout.sim_transit_ms") == 400);
  rig.turnout().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.offline") == 0);

  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  rig.tick(399);
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
  rig.tick(400);
  CHECK(rig.turnout().state() == TurnoutState::Reverse);

  rig.turnout().setAttribute("turnout.sim_transit_ms", SetOp::Assign, AttrValue::number(50));
  rig.turnout().exportConfig();
  CHECK(rec.lastValue(EventKind::Config, "turnout.sim_transit_ms") == 400);  // ONLINE again: locked
}

TURNOUT_SCENARIO("ONE_CONTACT reads Normal and emulates Reverse after sim_transit_ms") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().offline();
  rig.turnout().setAttribute("turnout.feedback_regime", SetOp::Assign,
                             AttrValue::enumerator(FeedbackRegime::OneContact));
  rig.turnout().online();
  CHECK(rig.turnout().state() == TurnoutState::Normal);
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  rig.tick(10);
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
  rig.tick(1000);  // default sim_transit_ms; the points are still moving
  CHECK(rig.turnout().state() == TurnoutState::Reverse);
  CHECK(rig.driver().points() == Points::Between);
  rig.tick(kTravelMs);
  rig.turnout().setAlignment(TurnoutCommand::Normal);
  rig.tick(kTravelMs + 1000);
  CHECK(rig.turnout().state() == TurnoutState::InTransit);  // Normal needs the real contact
  rig.tick(2 * kTravelMs);
  CHECK(rig.turnout().state() == TurnoutState::Normal);
}

TURNOUT_SCENARIO("INCREMENT and DECREMENT nudge numbers and reject out-of-range results") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().offline();
  rig.turnout().setAttribute("turnout.sim_transit_ms", SetOp::Increment, AttrValue::number(250));
  CHECK(rec.lastValue(EventKind::Config, "turnout.sim_transit_ms") == 1250);
  rec.clear();
  rig.turnout().setAttribute("turnout.sim_transit_ms", SetOp::Decrement, AttrValue::number(2000));
  rig.turnout().setAttribute("maintenance.post", SetOp::Increment, AttrValue::number(1));
  rig.turnout().setAttribute("maintenance.offline", SetOp::Assign, AttrValue::boolean(false));  // RO
  CHECK(rec.events.empty());
}

TURNOUT_SCENARIO("EXPORT CONFIG and EXPORT ATTRIBUTES report every attribute once") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().exportConfig();
  const Recorder::Event* config = rec.last(EventKind::Config);
  CHECK(config && config->data.count == 3);
  CHECK(rec.lastValue(EventKind::Config, "maintenance.post") == 0);
  CHECK(rec.lastValue(EventKind::Config, "turnout.feedback_regime") ==
        static_cast<uint32_t>(FeedbackRegime::TwoContact));
  CHECK(rec.lastValue(EventKind::Config, "turnout.sim_transit_ms") == 1000);
  rig.turnout().exportAttributes();
  const Recorder::Event* attrs = rec.last(EventKind::Attributes);
  CHECK(attrs && attrs->data.count == 4);
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.id") == 0);
}

TURNOUT_SCENARIO("cycle runs only OFFLINE and throws there and back with dwell") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().cycle(1, 500);  // ONLINE: ignored
  rig.tick(1);
  CHECK(rig.driver().driveCount() == 0);

  rig.turnout().offline();
  rig.turnout().cycle(1, 500);
  rec.clear();
  uint32_t t = 1;
  for (; t <= 10000; t += 50) rig.tick(t);
  CHECK(rec.timeline == "IN_TRANSIT REVERSE IN_TRANSIT NORMAL ");
  CHECK(rig.driver().driveCount() == 2);
  CHECK(rig.turnout().state() == TurnoutState::Normal);
}

TURNOUT_SCENARIO("identify pulses the beacon and never moves the points") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().identify(IdentifyMode::Pulse, 300);
  CHECK(rig.driver().beacon());
  rig.turnout().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.id") == 1);
  rig.tick(299);
  CHECK(rig.driver().beacon());
  rig.tick(300);
  CHECK(!rig.driver().beacon());
  rig.turnout().identify(IdentifyMode::On, 0);
  rig.tick(5000);
  CHECK(rig.driver().beacon());
  rig.turnout().identify(IdentifyMode::Off, 0);
  CHECK(!rig.driver().beacon());
  CHECK(rig.driver().driveCount() == 0);
}

TURNOUT_SCENARIO("out-of-range NV config raises CONFIG_ERROR and boots on defaults") {
  Recorder rec;
  rig.nv().save("turnout.feedback_regime", 7);
  rig.turnout().setObserver(&rec);
  rig.tick(0);
  CHECK(rec.timeline == "Fault NORMAL Config ");
  CHECK(rec.last(EventKind::Fault)->fault == FaultType::ConfigError);
  CHECK(rec.lastValue(EventKind::Config, "turnout.feedback_regime") ==
        static_cast<uint32_t>(FeedbackRegime::TwoContact));
}

// ---- review findings: each of these failed before its fix ------------------

static void useRegime(TurnoutRig& rig, FeedbackRegime regime) {
  rig.turnout().offline();
  rig.turnout().setAttribute("turnout.feedback_regime", SetOp::Assign, AttrValue::enumerator(regime));
  rig.turnout().online();
}

TURNOUT_SCENARIO("a command before power-on is dropped and never reaches the motor") {
  Recorder rec;
  rig.turnout().setObserver(&rec);
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  rig.turnout().identify(IdentifyMode::On, 0);
  CHECK(rig.driver().driveCount() == 0);
  rig.tick(0);
  rig.tick(2 * kTravelMs);
  CHECK(rig.driver().driveCount() == 0);
  CHECK(rig.driver().motor() == TurnoutCommand::Unchanged);
  CHECK(!rig.driver().beacon());
  CHECK(rig.turnout().state() == TurnoutState::Normal);
}

TURNOUT_SCENARIO("online() during a throw de-energizes the motor and reports what is there") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().offline();
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  rig.tick(500);
  rig.turnout().online();
  CHECK(rig.driver().motor() == TurnoutCommand::Unchanged);
  rig.tick(3 * kTravelMs);
  CHECK(rig.driver().points() == Points::Between);
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
}

TURNOUT_SCENARIO("under SIMULATED, re-sending the command does not hold the turnout IN_TRANSIT") {
  Recorder rec;
  boot(rig, rec);
  useRegime(rig, FeedbackRegime::Simulated);
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  for (uint32_t t = 250; t < 1000; t += 250) {
    rig.tick(t);
    rig.turnout().setAlignment(TurnoutCommand::Reverse);
  }
  rig.tick(1000);
  CHECK(rig.turnout().state() == TurnoutState::Reverse);
}

TURNOUT_SCENARIO("a settled SIMULATED turnout stays put while the clock wraps past 2^32") {
  Recorder rec;
  boot(rig, rec);
  useRegime(rig, FeedbackRegime::Simulated);
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  rig.tick(1000);
  CHECK(rig.turnout().state() == TurnoutState::Reverse);
  rec.clear();
  for (uint32_t i = 1; i < 16; ++i) rig.tick(i << 28);
  rig.tick(0xFFFFFFFFu);
  rig.tick(0);  // 2^32 ms after the command: every timer reads small again
  rig.tick(500);
  rig.tick(999);
  rig.tick(5000);
  CHECK(rec.timeline.empty());
  CHECK(rig.turnout().state() == TurnoutState::Reverse);
}

TURNOUT_SCENARIO("a short that persists across restarts counts as one fault") {
  Recorder rec;
  boot(rig, rec);
  rig.driver().shortContacts();
  rig.tick(10);
  rig.turnout().online();
  rig.turnout().online();
  CHECK(rec.count(EventKind::Fault) == 3);  // reported on every re-verification
  rig.turnout().exportAttributes();
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.fault_count") == 1);
  CHECK(rec.lastValue(EventKind::Attributes, "maintenance.faulted") == 1);
}

TURNOUT_SCENARIO("changing feedback_regime OFFLINE forgets what the old regime derived") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().setAlignment(TurnoutCommand::Reverse);
  rig.tick(kTravelMs);
  CHECK(rig.turnout().state() == TurnoutState::Reverse);
  rig.turnout().offline();
  rig.turnout().setAttribute("turnout.feedback_regime", SetOp::Assign,
                             AttrValue::enumerator(FeedbackRegime::OneContact));
  rig.tick(kTravelMs + 10);
  // ONE_CONTACT proves Reverse only by its own emulated throw.
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
  rig.turnout().online();
  CHECK(rig.turnout().state() == TurnoutState::InTransit);
}

TURNOUT_SCENARIO("IMPORT is all-or-nothing: one bad entry changes nothing and emits no CONFIG") {
  Recorder rec;
  boot(rig, rec);
  rig.turnout().offline();
  const AttributeInfo* post = &kTurnoutAttributes[kTurnoutAttributeTable.indexOf("maintenance.post")];
  const AttributeInfo* regime = &kTurnoutAttributes[kTurnoutAttributeTable.indexOf("turnout.feedback_regime")];
  const AttributeInfo* faulted = &kTurnoutAttributes[kTurnoutAttributeTable.indexOf("maintenance.faulted")];
  AttrMap bad_enum, bad_bool, not_config;
  bad_enum.add(post, AttrValue{1});
  bad_enum.add(regime, AttrValue{9});  // past the last FeedbackRegime
  bad_bool.add(post, AttrValue{1});
  bad_bool.add(post, AttrValue{2});
  not_config.add(post, AttrValue{1});
  not_config.add(faulted, AttrValue{0});  // volatile: not importable
  for (const AttrMap* m : {&bad_enum, &bad_bool, &not_config}) {
    rec.clear();
    rig.turnout().importConfig(*m);
    CHECK(rec.count(EventKind::Config) == 0);
  }
  uint32_t v = 0;
  CHECK(!rig.nv().load("maintenance.post", v));
  AttrMap good;
  good.add(post, AttrValue{1});
  rig.turnout().importConfig(good);
  CHECK(rec.lastValue(EventKind::Config, "maintenance.post") == 1);
}

int main() { return codeline::scenario::run(); }
