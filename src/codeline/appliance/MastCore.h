// Reference Appliance Core for Appliance:SIGNALMAST: Cluster:MAINTENANCE
// plus Cluster:MAST over any IMastDriver. Rulebook translation (indication ->
// per-head aspects) and flash timing live here, below the seam; the engine
// only ever says setIndication().
//
// Sources: docs/clusters/README.Clusters.md (MAST),
// docs/appliances/README.Appliances.md (Appliance:SIGNALMAST); the aspects
// come from schema/rulebooks/ via codeline/generated/Rulebooks.h.
//
// STATE (README.Clusters.md Cluster:MAST, `indication`): state() is the last
// *confirmed* { indication, lit }. A new state is emitted only when every head
// (head.A .. mast.heads) proves the aspect it was given, as the driver judges
// it (servo travel or lamp animation finished; see IMastDriver::headProven).
// While heads move, the prior confirmed state is held and nothing is emitted,
// for every change: to STOP, to a proceed, proceed to proceed, and lit/dark.
// So a STOP report always means STOP proven on every head.
//
// Power-on: the heads are driven to STOP and lit, and nothing is reported
// until STOP is proven; the RESTART order becomes CONFIG first, then (once
// proven) the state. Before that first proof state() returns the declared
// Startup State { STOP, lit } *unproven*: no observer has been told it, so an
// ApplianceHealth binding never sees a state, never asks for attributes, and
// reads Unproven, never Ok. (Over MQTT the adapter holds /state at
// UNAVAILABLE until the first emission; see MqttAdapterBase.)
// ONLINE restart: the same, except the held state is whatever was confirmed
// before the restart (it may be a proceed) until STOP is proven; the
// OnlineEvent comes before that proof. A restart always reports the proven
// Startup State, even when it equals the held one.
//
// Head failure: when the driver reports a head failed (headFailed(), as
// opposed to "not yet proven"), the mast raises FaultEvent HW_ERROR once per
// failure episode and keeps holding the prior confirmed state: it never
// emits STOP for a failure. A head that never proves and never reports
// failure holds the prior state forever: there is no timer in this core (no
// STALL on masts). clearFault() clears only while no head reports failure;
// restart re-verifies (a failure found again is not re-counted).
//
// A demand the rulebook has no row for (for this many heads) is displayed as
// the STOP row and confirmed as STOP. The generator guarantees every rulebook
// a STOP row for every head count mast.heads allows.
//
// Flashing: a flashing head alternates its color and NONE every half period
// of mast.flash_hz while lit. While dark (lit = false) heads hold their color
// steady (searchlight mechanisms and blades still take their positions) and
// the lamps are off.
//
// Proof of a flashing head comes only from its lit half: NONE proves nothing
// about the lamp that should be showing the color. The last lit-half reading
// (proven, failed) is held through the dark half; a newly shown aspect (or a
// change of approach lighting) is unproven until its first lit half proves
// it, so the prior confirmed state is held until then. While dark, a
// flashing head shows its color steady and is proven directly.
//
// state().lit is the approach lighting confirmed with the indication: a
// lit/dark change is reported once the heads prove under it. The port has no
// proof of illumination beyond headProven().
//
// cycle(count, dwell): steps through the rulebook's rows for mast.heads in
// table order, holding each for dwell_ms once proven, for `count` passes,
// then returns to STOP. An operational command ends a cycle.
#pragma once

#include <cstdint>

#include "codeline/appliance/ISignalMast.h"
#include "codeline/appliance/MaintenanceCore.h"
#include "codeline/generated/Rulebooks.h"
#include "codeline/generated/SignalmastAppliance.h"

namespace codeline {

static_assert(kSignalmastAttributes[static_cast<int>(SignalmastAttr::MastFlashHz)].type == AttrType::Hundredths,
              "MastCore's flash timing reads mast.flash_hz as hundredths");
static_assert(kSignalmastAttributes[static_cast<int>(SignalmastAttr::MastFlashHz)].min_value > 0,
              "a flash rate of zero has no half period");
static_assert(kSignalmastAttributes[static_cast<int>(SignalmastAttr::MastHeads)].max_value <= kMaxHeads,
              "mast.heads cannot exceed the rulebook table width");

class MastCore final : public MaintenanceCore<IMast, IMastObserver, SignalmastSchema> {
public:
  MastCore(IMastDriver& driver, INvStore& nv) : MaintenanceCore(nv), driver_(driver) {}

  // ---- Cluster:MAST ------------------------------------------------------

  // Operational: accepted ONLINE and OFFLINE.
  void setIndication(SignalIndication indication, bool lit) override {
    if (!booted()) return;
    cycle_.active = false;
    applyLit(lit);
    show(indication);
    // Every command is answered with a state report once the heads are
    // proven, also when the state is the one last reported (owner ruling):
    // the controller takes proof of STOP only from a report that follows its
    // STOP command, so a mast that ends where it began must still say so.
    report_due_ = true;
  }

  void setLit(bool lit) override {
    if (!booted()) return;
    applyLit(lit);
  }

  MastState state() const override { return reported_; }

private:
  struct Cycle {
    bool active = false;
    uint16_t passes_left = 0;
    uint8_t row = 0;  // index into the rulebook table
    uint32_t dwell_ms = 0;
    bool dwelling = false;
  };

  IMastDriver& driver_;
  MastState reported_;  // { STOP, lit } until proven otherwise
  bool lit_ = true;
  SignalIndication demand_ = SignalIndication::Stop;  // last demanded
  SignalIndication shown_ = SignalIndication::Stop;   // what the heads were given
  HeadAspect aspect_[kMaxHeads] = {};
  bool lit_half_proven_[kMaxHeads] = {};  // flashing heads: proof from the last lit half
  bool lit_half_failed_[kMaxHeads] = {};  // flashing heads: failure from the last lit half
  bool report_due_ = false;               // restart or a command: report the state once proven, changed or not
  bool failure_raised_ = false;           // HW_ERROR raised for the current failure episode
  bool flash_on_ = true;
  ElapsedMillis flash_timer_{clock_};  // read only while a lit head flashes
  ElapsedMillis dwell_timer_{clock_};  // read only while cycle_.dwelling
  Cycle cycle_;

  uint8_t heads() const { return static_cast<uint8_t>(get(Attr::MastHeads)); }
  const RulebookTable& rulebook() const { return kRulebooks[get(Attr::MastRulebook)]; }

  const RulebookRow* find(SignalIndication ind) const {
    const RulebookTable& t = rulebook();
    for (uint8_t i = 0; i < t.count; ++i) {
      if (t.rows[i].heads == heads() && t.rows[i].indication == ind) return &t.rows[i];
    }
    return nullptr;
  }

  // ---- heads ----

  void show(SignalIndication demand) {
    demand_ = demand;
    const RulebookRow* row = find(demand);
    if (!row) row = find(SignalIndication::Stop);
    shown_ = row ? row->indication : SignalIndication::Stop;
    for (uint8_t h = 0; h < kMaxHeads; ++h) {
      if (h >= heads()) aspect_[h] = HeadAspect{ColorName::None, false};
      else aspect_[h] = row ? row->aspect[h] : HeadAspect{ColorName::Red, false};
    }
    restartFlash();
  }

  // Back to the lit half of the cadence; flashing heads unproven until it proves.
  void restartFlash() {
    flash_on_ = true;
    flash_timer_ = 0;
    for (bool& p : lit_half_proven_) p = false;
    for (bool& f : lit_half_failed_) f = false;
    driveHeads();
  }

  void driveHeads() {
    for (uint8_t h = 0; h < kMaxHeads; ++h) {
      // flash_on_ stays true while the mast is dark (applyLit, updateFlash).
      const bool dark_phase = aspect_[h].flashing && !flash_on_;
      driver_.setHead(h, dark_phase ? ColorName::None : aspect_[h].color);
    }
  }

  bool flashing() const {
    for (uint8_t h = 0; h < heads(); ++h) {
      if (aspect_[h].flashing) return true;
    }
    return false;
  }

  void applyLit(bool lit) {
    if (lit == lit_) return;
    lit_ = lit;
    driver_.setLamps(lit);
    restartFlash();  // a dark mast holds its colors steady
  }

  void updateFlash() {
    if (!lit_ || !flashing()) return;
    // mast.flash_hz is in hundredths of a hertz (schema bounds: 0.01 .. 10 Hz).
    const uint32_t half_ms = 500u * kHundredthsPerUnit / get(Attr::MastFlashHz);
    if (flash_timer_ < half_ms) return;
    // Drift-free cadence; after a long gap between ticks, start over instead of catching up.
    if (flash_timer_ >= 2 * half_ms) flash_timer_ = 0;
    else flash_timer_ -= half_ms;
    flash_on_ = !flash_on_;
    driveHeads();
  }

  // A flashing head on a lit mast is proven only by its lit half (see above).
  bool proven() {
    bool all = true;
    for (uint8_t h = 0; h < heads(); ++h) {
      if (aspect_[h].flashing && lit_) {
        if (flash_on_) lit_half_proven_[h] = driver_.headProven(h);
        all = all && lit_half_proven_[h];
      } else {
        all = all && driver_.headProven(h);
      }
    }
    return all;
  }

  // Whether any head in use reports failure; flashing lit heads from their lit half.
  bool anyHeadFailed() {
    bool any = false;
    for (uint8_t h = 0; h < heads(); ++h) {
      if (aspect_[h].flashing && lit_) {
        if (flash_on_) lit_half_failed_[h] = driver_.headFailed(h);
        any = any || lit_half_failed_[h];
      } else {
        any = any || driver_.headFailed(h);
      }
    }
    return any;
  }

  // Emits only a proven state; holds the prior confirmed one otherwise.
  void evaluate() {
    if (anyHeadFailed()) {
      if (!failure_raised_) {
        failure_raised_ = true;
        raiseFault<FaultType::HwError>("signal head failed: aspect cannot be proven");
      }
    } else {
      failure_raised_ = false;  // episode over; a new failure raises again
    }
    if (!proven()) return;
    const MastState now{shown_, lit_};
    if (now == reported_ && !report_due_) return;
    report_due_ = false;
    reported_ = now;
    if (observer_) observer_->onStateChanged(reported_);
  }

  // ---- cycle ----

  void startCycle(uint16_t count, uint32_t dwell_ms) override {
    const RulebookTable& t = rulebook();
    uint8_t first = t.count;
    for (uint8_t i = 0; i < t.count && first == t.count; ++i) {
      if (t.rows[i].heads == heads()) first = i;
    }
    if (first == t.count) return;  // nothing this mast can step through
    cycle_ = Cycle{true, count, first, dwell_ms, false};
    show(t.rows[first].indication);
  }

  void updateCycle() {
    if (!cycle_.active || reported_.indication != shown_ || !proven()) return;
    if (!cycle_.dwelling) {
      cycle_.dwelling = true;
      dwell_timer_ = 0;
      return;
    }
    if (dwell_timer_ < cycle_.dwell_ms) return;
    cycle_.dwelling = false;
    const RulebookTable& t = rulebook();
    uint8_t next = cycle_.row + 1;
    while (next < t.count && t.rows[next].heads != heads()) ++next;
    if (next == t.count) {
      if (--cycle_.passes_left == 0) {
        cycle_.active = false;
        show(SignalIndication::Stop);
        return;
      }
      next = 0;
      while (t.rows[next].heads != heads()) ++next;
    }
    cycle_.row = next;
    show(t.rows[next].indication);
  }

  // ---- MaintenanceCore hooks ----

  void stopActivity() override { cycle_.active = false; }

  // Restart step 3. Reads the heads as they are (still showing the aspect
  // from before the restart).
  void verifyHardware() override {
    failure_raised_ = anyHeadFailed();
    if (failure_raised_) raiseFault<FaultType::HwError>("signal head failed: aspect cannot be proven");
  }

  bool faultConditionPresent() const override {
    for (uint8_t h = 0; h < heads(); ++h) {
      if (driver_.headFailed(h)) return true;
    }
    return false;
  }

  // Drives STOP, lit. The report waits for proof (see the file comment).
  void enterStartup() override {
    lit_ = true;
    driver_.setLamps(true);
    show(SignalIndication::Stop);
    report_due_ = true;
    evaluate();
  }

  void tickDomain() override {
    updateFlash();
    evaluate();
    updateCycle();
  }

  // A new head count or rulebook changes what the heads must show.
  void onStored(int index) override {
    if (index == idx(Attr::MastHeads) || index == idx(Attr::MastRulebook)) {
      cycle_.active = false;
      show(demand_);
    }
  }

  void driveBeacon(bool on) override { driver_.setIdentifyBeacon(on); }
};

}  // namespace codeline
