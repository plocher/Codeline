// An electric switch lock: one Appliance:CONTROL in the "Electric Switch
// Lock" profile (docs/appliances/README.Appliances.md, CONTROL usage
// profiles; docs/bindings/MQTT.md section 5a):
//   SW1      lock open / selector in HAND (a crew has the switch)
//   BTN1     crew holds the release request button
//   LAMP1    the field grants release (release indicator lit)
//
// The donor FieldUnit engine has no appliance for this: it only toggles a
// HAND_LOCKED bit on the switch when it accepts WLS. This is the minimum the
// Walkthrough move needs: routes over the locked switches require the lock
// proven locked, and release is granted only while nothing is lined, locked,
// or not proven at STOP over those switches (checked by the plant).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "codeline/appliance/IControl.h"
#include "codeline/core/TickClock.h"
#include "codeline/engine/ApplianceLink.h"

namespace codeline::engine {

class Switch;

class ElectricLock {
public:
  // `bind` false: a detached lock that never observes the CONTROL (a
  // configuration the plant refused).
  ElectricLock(std::string_view name, IControl& control, const TickClock& clock, bool bind = true)
      : name_(name), control_(control), link_(control, bind), since_withdrawal_(clock) {}

  ElectricLock(const ElectricLock&) = delete;
  ElectricLock& operator=(const ElectricLock&) = delete;

  const std::string& name() const { return name_; }
  const std::vector<Switch*>& switches() const { return switches_; }

  // Vital: the lock is proven closed and the release is proven withdrawn.
  // Unhealthy reads unlocked (Codeline.md, Executive-Owned Vital Fail-Safe
  // Substitution: CONTROL -> WLK asserted): losing the appliance forgets
  // the withdrawal, which is proven again only after re-admission.
  //
  // "Withdrawn" (README.CodeLine 5.1 rule 3, review finding 4): the release
  // lamp off was sent in an earlier scan, at least withdrawalMs() ago, with
  // no grant and no loss of the appliance since, and this scan's sample
  // reads the contact closed. IControl confirms nothing about its lamps, so
  // a CONTROL that takes longer than withdrawalMs() to put the lamp out
  // still leaves a window in which a crew can open the lock under a clear
  // signal; the next sample that reads it open drops the signal, with
  // approach locking. Author choice: withdrawalMs() 1000 (the
  // ApplianceHealth retry period); the docs give no CONTROL latency.
  bool provenLocked() const { return !open_ && withdrawn_; }
  bool granted() const { return granted_; }
  bool requested() const { return request_; }

  // Informative (debug channel); no vital logic reads these.
  Health applianceHealth() const { return link_.health.health(); }
  bool healthy() const { return healthy_; }
  bool open() const { return open_; }
  // The release lamp off was sent and is not yet old enough to count as withdrawn.
  bool withdrawing() const { return withdrawal_sent_ && !withdrawn_; }
  uint32_t withdrawalElapsedMs() const { return since_withdrawal_; }

  uint32_t withdrawalMs() const { return withdrawal_ms_; }
  void setWithdrawalMs(uint32_t ms) { withdrawal_ms_ = ms; }

  // Indication lock/RELEASED: asserted whenever the lock is not proven
  // locked (granted, open, or unhealthy). See the report: README.CodeLine
  // calls "dropped" the fail-safe state; Codeline.md substitutes WLK
  // asserted. The engine follows Codeline.md.
  bool releasedIndication() const { return !provenLocked(); }

  // Unproven contacts (count != 1, the lock profile's lever.inputs) read
  // open.
  void sample(uint32_t now_ms) {
    const bool healthy = admit(link_.health, control_, now_ms);
    if (healthy != healthy_) {
      resend_ = healthy;          // (re-)admission: state the lamp again
      withdrawal_sent_ = false;   // whatever was sent may not have arrived
    }
    healthy_ = healthy;
    const BitVector sw = control_.state();
    open_ = sw.count != 1 || sw.get(1);
    request_ = healthy && link_.buttonHeld(1);
    withdrawn_ = withdrawal_sent_ && static_cast<uint32_t>(since_withdrawal_) >= withdrawal_ms_;
  }

  void grant(bool g) {
    if (g) {
      withdrawal_sent_ = false;
      withdrawn_ = false;
    }
    granted_ = g;
  }

  void drive() {
    if (!healthy_) return;
    // Nothing to say again, unless the lock stands not granted with no
    // withdrawal on record: a release granted and taken back between two
    // scans never lit the lamp, but grant() forgot the withdrawal, so the
    // lamp is put out once more and the withdrawal is timed afresh.
    if (!resend_ && granted_ == last_sent_ && (granted_ || withdrawal_sent_)) return;
    BitVector lamps;
    lamps.count = 1;
    lamps.set(1, granted_);
    control_.setLamps(lamps);
    last_sent_ = granted_;
    resend_ = false;
    if (!granted_) {
      withdrawal_sent_ = true;
      since_withdrawal_ = 0;
    }
  }

private:
  friend class Plant;  // fills switches_ at configuration

  std::string name_;
  IControl& control_;
  ControlLink link_;
  std::vector<Switch*> switches_;
  bool healthy_ = false;
  bool open_ = true;
  bool request_ = false;
  bool granted_ = false;  // restart: locks locked (README.CodeLine 5.1 rule 3)
  bool last_sent_ = false;
  bool resend_ = false;  // set on every (re-)admission, the first included
  bool withdrawal_sent_ = false;  // lamp off sent, nothing granted or lost since
  bool withdrawn_ = false;        // ... and long enough ago, as of this scan's sample
  ElapsedMillis since_withdrawal_;
  uint32_t withdrawal_ms_ = 1000;
};

}  // namespace codeline::engine
