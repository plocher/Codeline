// CP Corporal: the Walkthrough's westbound move (docs/Walkthrough.md section
// 1), the cancel with time locking, and the electric lock 827 with its
// derail 827D, on a plant built by hand (codeline/testing/CorporalPlant.h).
//
// Frames are spelled with the AAR tokens of the derived chart
// (uv run python tools/codeline_chart.py --station Corporal), copied below in
// chart order. The scenario maps each chart entry to the engine's function
// model, as a later chart mapper will: (class, appliance, word).
#include <string>
#include <vector>

#include "codeline/testing/CorporalPlant.h"
#include "scenario.h"

using namespace codeline;
using namespace codeline::testing;
using engine::ControlResult;
using engine::FunctionClass;
using engine::Position;
using engine::Word;

namespace {

struct ChartEntry {
  FunctionClass cls;
  const char* appliance;
  Word word;
  const char* aar;
};

// Chart 85973c19, station Corporal.
const ChartEntry kControls[] = {
    {FunctionClass::Switch, "829", Word::Normal, "829NWS"},
    {FunctionClass::Switch, "829", Word::Reverse, "829RWS"},
    {FunctionClass::Lock, "827", Word::Release, "827WLS"},
    {FunctionClass::Signal, "828", Word::Left, "828WGS"},
    {FunctionClass::Signal, "828", Word::Right, "828EGS"},
    {FunctionClass::Signal, "828", Word::Stop, "828HS"},
    {FunctionClass::Signal, "830", Word::Left, "830WGS"},
    {FunctionClass::Signal, "830", Word::Right, "830EGS"},
    {FunctionClass::Signal, "830", Word::Stop, "830HS"},
    {FunctionClass::MaintainerCall, "1MC", Word::Call, "1MCS"},
};
const ChartEntry kIndications[] = {
    {FunctionClass::Switch, "829", Word::Normal, "829NWK"},
    {FunctionClass::Switch, "829", Word::Reverse, "829RWK"},
    {FunctionClass::Lock, "827", Word::Released, "827WLK"},
    {FunctionClass::Lock, "827", Word::Request, "827WLQK"},
    {FunctionClass::Signal, "828", Word::Left, "828WGK"},
    {FunctionClass::Signal, "828", Word::Right, "828EGK"},
    {FunctionClass::Signal, "828", Word::Time, "828TEK"},
    {FunctionClass::Signal, "830", Word::Left, "830WGK"},
    {FunctionClass::Signal, "830", Word::Right, "830EGK"},
    {FunctionClass::Signal, "830", Word::Time, "830TEK"},
    {FunctionClass::MaintainerCall, "1MC", Word::Call, "1MCK"},
    {FunctionClass::TrackCircuit, "827T1", Word::Occupied, "827T1K"},
    {FunctionClass::TrackCircuit, "829T1", Word::Occupied, "829T1K"},
};

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// A complete control frame in chart order; bare token = asserted.
ControlResult send(CorporalPlant& p, const std::string& frame) {
  engine::ControlTransaction c = p.cp.newControls();
  const std::vector<std::string> tokens = split(frame);
  CHECK(tokens.size() == sizeof(kControls) / sizeof(kControls[0]));
  for (size_t i = 0; i < tokens.size() && i < sizeof(kControls) / sizeof(kControls[0]); ++i) {
    const bool asserted = tokens[i].front() != '(';
    const std::string name = asserted ? tokens[i] : tokens[i].substr(1, tokens[i].size() - 2);
    CHECK(name == kControls[i].aar);
    if (asserted) c.assertFunction(kControls[i].cls, kControls[i].appliance, kControls[i].word);
  }
  return p.cp.apply(c);
}

// The complete indication frame in chart order.
std::string indications(const CorporalPlant& p) {
  const engine::Indications ind = p.cp.indications();
  std::string out;
  for (const ChartEntry& e : kIndications) {
    bool v = false;
    const bool known = ind.get(e.cls, e.appliance, e.word, v);
    CHECK(known);
    if (!out.empty()) out += ' ';
    out += v ? std::string(e.aar) : "(" + std::string(e.aar) + ")";
  }
  return out;
}

const char* kClearFrame = "829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)";
const char* kCancelFrame = "829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) (830WGS) (830EGS) 830HS (1MCS)";

// Boots Corporal (829 found reversed) and waits out the startup time locking
// of the routes whose approach is outside this plant (30 s).
void bootAndSettle(CorporalPlant& p) {
  p.boot();
  p.run(31000);
}

}  // namespace

SCENARIO("Corporal startup: every signal at STOP, unproven routes time-locked, then at rest") {
  CorporalPlant p;
  p.boot();
  p.step();
  CHECK(p.cp.indications().is(FunctionClass::Signal, "830", Word::Time));  // 830E: approach unknown
  CHECK(!p.sw829.WLR());
  p.run(31000);
  CHECK(indications(p) ==
        "(829NWK) 829RWK (827WLK) (827WLQK) (828WGK) (828EGK) (828TEK) (830WGK) (830EGK) (830TEK) (1MCK) (827T1K) "
        "(829T1K)");
  CHECK(p.sw829.WLR());
}

SCENARIO("Corporal Walkthrough: the westbound move, in the Walkthrough's order") {
  CorporalPlant p;
  bootAndSettle(p);
  // Step 1: the dispatcher's frame. Step 2: OS vacant, lock 827 locked,
  // 827D derailing, opposing 830EAB and 828EA at STOP -> accepted.
  CHECK(p.mast830EAB.provenStop() && p.mast828EA.provenStop());
  CHECK(p.el827.provenLocked() && p.sw827D.provenAt(Position::Normal));
  CHECK(send(p, kClearFrame) == ControlResult::Applied);

  uint32_t cmd829 = 0, transit829 = 0, normal829 = 0, cmdMast = 0, approachShown = 0;
  bool step4Seen = false, step7Seen = false;
  const uint32_t t0 = p.t;
  while (p.t < t0 + 5000) {
    p.step();
    if (!cmd829 && p.t829.commands() > 0) cmd829 = p.t;
    if (!transit829 && p.t829.state() == TurnoutState::InTransit) transit829 = p.t;
    if (!normal829 && p.t829.state() == TurnoutState::Normal) normal829 = p.t;
    if (!cmdMast && p.m830WAB.lastCommand() == SignalIndication::Approach) cmdMast = p.t;
    if (!approachShown && p.m830WAB.state().indication == SignalIndication::Approach) approachShown = p.t;
    const std::string ind = indications(p);
    // Step 4: points unlocked and moving; panel 829 lamps dark.
    if (ind == "(829NWK) (829RWK) (827WLK) (827WLQK) (828WGK) (828EGK) (828TEK) (830WGK) (830EGK) (830TEK) "
               "(1MCK) (827T1K) (829T1K)")
      step4Seen = true;
    // Step 7: the mast confirms APPROACH.
    if (ind == "829NWK (829RWK) (827WLK) (827WLQK) 828WGK (828EGK) (828TEK) 830WGK (830EGK) (830TEK) (1MCK) "
               "(827T1K) (829T1K)")
      step7Seen = true;
  }
  CHECK(p.t829.lastCommand() == TurnoutCommand::Normal);  // step 3
  CHECK(cmd829 != 0 && transit829 >= cmd829);            // step 4
  CHECK(normal829 > transit829);                         // step 5
  CHECK(cmdMast >= normal829);                           // step 6: APPROACH, the next signal is at STOP
  CHECK(approachShown > cmdMast);                        // step 7
  CHECK(step4Seen);
  CHECK(step7Seen);
  CHECK(p.mast830EAB.demanded() == SignalIndication::Stop);
  CHECK(p.mast828EA.demanded() == SignalIndication::Stop);
  CHECK(p.mast828WA.demanded() == SignalIndication::Stop);
}

SCENARIO("Corporal Walkthrough: cancel 830 with a train approaching starts time locking on proof of STOP") {
  CorporalPlant p;
  bootAndSettle(p);
  CHECK(send(p, kClearFrame) == ControlResult::Applied);
  p.run(3000);
  CHECK(p.m830WAB.state().indication == SignalIndication::Approach);
  p.s832T.shunt(true);  // the freight is on the approach circuit
  p.run(300);
  p.m830WAB.setConfirmMs(1500);  // a slow mast makes the order visible
  CHECK(send(p, kCancelFrame) == ControlResult::Applied);
  p.step();
  CHECK(p.mast830WAB.demanded() == SignalIndication::Stop);  // /signalmast/830WAB/cmd <- STOP
  CHECK(!p.cp.indications().is(FunctionClass::Signal, "830", Word::Time));
  CHECK(p.cp.indications().is(FunctionClass::Signal, "830", Word::Left));  // still displaying APPROACH
  CHECK(!p.sw829.WLR());
  uint32_t proof = 0;
  while (!proof && p.t < 1000000) {
    p.step();
    if (p.mast830WAB.provenStop()) proof = p.t;
  }
  CHECK(proof != 0);
  // Author's reading of README.CodeLine 3: 828WGK is dropped once no 828
  // route shows proceed; the Walkthrough's cancel frame shows 828WGK lit.
  CHECK(indications(p) ==
        "829NWK (829RWK) (827WLK) (827WLQK) (828WGK) (828EGK) (828TEK) (830WGK) (830EGK) 830TEK (1MCK) (827T1K) "
        "(829T1K)");
  // 829WLR stays down until the TER expires, 30 s from the proof.
  CHECK(send(p, "(829NWS) 829RWS (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) 830HS (1MCS)") ==
        ControlResult::VitalIgnored);
  p.run(proof + 29800 - p.t);
  CHECK(!p.sw829.WLR());
  CHECK(p.t829.lastCommand() == TurnoutCommand::Normal);
  p.run(400);
  CHECK(p.sw829.WLR());
  CHECK(!p.cp.indications().is(FunctionClass::Signal, "830", Word::Time));
  CHECK(send(p, "(829NWS) 829RWS (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) 830HS (1MCS)") ==
        ControlResult::Applied);
  p.run(2500);
  CHECK(p.sw829.provenAt(Position::Reverse));
}

SCENARIO("Corporal: the train accepts 830WAB and releases the plant section by section") {
  CorporalPlant p;
  bootAndSettle(p);
  p.s832T.shunt(true);
  CHECK(send(p, kClearFrame) == ControlResult::Applied);
  p.run(3000);
  CHECK(p.m830WAB.state().indication == SignalIndication::Approach);
  p.s829T1.shunt(true);  // enters the OS
  p.step();
  CHECK(p.mast830WAB.demanded() == SignalIndication::Stop);
  CHECK(p.sig830.active() == engine::Direction::None);
  CHECK(p.sig828.active() == engine::Direction::None);
  p.s832T.shunt(false);
  p.s827T1.shunt(true);
  p.step();
  p.s829T1.shunt(false);
  p.run(500);
  CHECK(p.sw829.WLR());  // 829's section released behind the train
  CHECK(!p.cp.timeLocking(p.sig830));
  p.s827T1.shunt(false);
  p.run(500);
  CHECK(indications(p) ==
        "829NWK (829RWK) (827WLK) (827WLQK) (828WGK) (828EGK) (828TEK) (830WGK) (830EGK) (830TEK) (1MCK) (827T1K) "
        "(829T1K)");
}

SCENARIO("Corporal: opposing 828EA cannot clear against 830WAB") {
  CorporalPlant p;
  bootAndSettle(p);
  CHECK(send(p, kClearFrame) == ControlResult::Applied);
  p.run(3000);
  CHECK(send(p, "829NWS (829RWS) (827WLS) (828WGS) 828EGS (828HS) (830WGS) 830EGS (830HS) (1MCS)") ==
        ControlResult::VitalIgnored);
  p.run(500);
  CHECK(p.m828EA.state().indication == SignalIndication::Stop);
  CHECK(p.m830WAB.state().indication == SignalIndication::Approach);
}

SCENARIO("Corporal lock 827: release, request lamp, hand throw, and re-lock") {
  CorporalPlant p;
  bootAndSettle(p);
  p.lock827.pressRequest(true);  // the crew asks
  p.step();
  CHECK(p.cp.indications().is(FunctionClass::Lock, "827", Word::Request));
  CHECK(send(p, "(829NWS) (829RWS) 827WLS (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)") ==
        ControlResult::Applied);
  p.run(300);  // the release lamp lights after the command latency
  CHECK(p.lock827.releaseLit());
  CHECK(p.cp.indications().is(FunctionClass::Lock, "827", Word::Released));
  p.lock827.pressRequest(false);
  CHECK(p.lock827.open(true));  // the crew opens the lock and lines the siding
  p.t827D.handThrow(TurnoutState::Reverse);
  p.t827.handThrow(TurnoutState::Reverse);
  p.run(300);
  const char* releasedAnd830 = "829NWS (829RWS) 827WLS 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)";
  // 830W does not line over 827 reverse: the frame is safe, the signal
  // demand is unfulfilled and dropped.
  CHECK(send(p, releasedAnd830) == ControlResult::Applied);
  p.run(2500);  // 829 throws normal; the demand waits for it, then drops
  CHECK(p.sw829.provenAt(Position::Normal));
  CHECK(p.m830WAB.state().indication == SignalIndication::Stop);
  CHECK(p.sig830.active() == engine::Direction::None);
  // Back to normal, lock still open: 830W lines over a released lock -> unsafe.
  p.t827.handThrow(TurnoutState::Normal);
  p.t827D.handThrow(TurnoutState::Normal);
  p.run(300);
  CHECK(send(p, releasedAnd830) == ControlResult::VitalIgnored);
  p.lock827.open(false);  // closed, but the field still grants release
  p.run(300);
  CHECK(!p.el827.provenLocked());
  CHECK(send(p, releasedAnd830) == ControlResult::VitalIgnored);
  // (827WLS) re-locks, but a frame that re-locks may not also clear over
  // the lock: the release is not yet proven withdrawn.
  CHECK(send(p, kClearFrame) == ControlResult::VitalIgnored);
  CHECK(send(p, "829NWS (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) (830EGS) (830HS) (1MCS)") ==
        ControlResult::Applied);
  p.step();  // the release lamp off is sent
  CHECK(!p.el827.provenLocked());
  CHECK(send(p, kClearFrame) == ControlResult::VitalIgnored);
  p.run(1500);  // the withdrawal is proven: sent, a withdrawal time ago, contact closed
  CHECK(!p.lock827.releaseLit());
  CHECK(p.el827.provenLocked());
  CHECK(send(p, kClearFrame) == ControlResult::Applied);
  p.run(3000);
  CHECK(p.m830WAB.state().indication == SignalIndication::Approach);
}

SCENARIO("Corporal lock 827: a release is unsafe while 830WAB is lined over 827") {
  CorporalPlant p;
  bootAndSettle(p);
  CHECK(send(p, kClearFrame) == ControlResult::Applied);
  p.run(3000);
  CHECK(send(p, "829NWS (829RWS) 827WLS 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)") ==
        ControlResult::VitalIgnored);
  p.run(300);
  CHECK(!p.lock827.releaseLit());
  CHECK(p.m830WAB.state().indication == SignalIndication::Approach);
}

SCENARIO("Corporal lock 827: release is allowed during the parallel move on MT2") {
  CorporalPlant p;
  bootAndSettle(p);
  CHECK(send(p, "(829NWS) (829RWS) (827WLS) (828WGS) (828EGS) (828HS) (830WGS) 830EGS (830HS) (1MCS)") ==
        ControlResult::Applied);
  p.run(800);
  CHECK(p.m830EAB.state().indication == SignalIndication::Approach);
  CHECK(send(p, "(829NWS) (829RWS) 827WLS (828WGS) (828EGS) (828HS) (830WGS) 830EGS (830HS) (1MCS)") ==
        ControlResult::Applied);
  p.run(300);
  CHECK(p.lock827.releaseLit());
  CHECK(p.m830EAB.state().indication == SignalIndication::Approach);
}

SCENARIO("Corporal lock 827: an unhealthy lock or a derail off the rail keeps 830WAB at STOP") {
  CorporalPlant p;
  bootAndSettle(p);
  p.lock827.setHealth(StandInHealth::Unavailable);
  p.step();
  CHECK(p.cp.indications().is(FunctionClass::Lock, "827", Word::Released));  // Codeline.md: WLK asserted
  CHECK(send(p, kClearFrame) == ControlResult::VitalIgnored);
  p.lock827.setHealth(StandInHealth::Ok);
  p.run(300);
  CHECK(p.cp.indications().is(FunctionClass::Lock, "827", Word::Released));  // withdrawal re-sent, not yet proven
  p.run(1200);
  CHECK(!p.cp.indications().is(FunctionClass::Lock, "827", Word::Released));

  p.t827D.handThrow(TurnoutState::Reverse);  // derail lifted (non-derailing)
  p.step();
  CHECK(send(p, kClearFrame) == ControlResult::Applied);  // nothing unsafe: no 830 route lines
  p.run(3000);
  CHECK(p.m830WAB.state().indication == SignalIndication::Stop);
  CHECK(p.sig830.active() == engine::Direction::None);  // unfulfilled: dropped
}

int main() { return codeline::scenario::run(); }
