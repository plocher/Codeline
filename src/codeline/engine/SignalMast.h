// A signal mast as vital logic sees it: one Appliance:SIGNALMAST, driven by
// an indication demand only. Aspects, heads and rulebooks are the mast
// appliance's business (spike section 4, change 4).
//
// The demand (what the engine commands) and the confirmed indication (what
// the appliance reports it displays) are separate. An unhealthy mast is
// substituted two ways (spike section 6, item 1):
// - confirmed() reads STOP: where the mast is read as an indication
//   (office GK, the next signal's DR);
// - provenStop() reads false: where the mast is read for locking (opposing
//   signals, start of the TER, release of route locks).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "codeline/appliance/ISignalMast.h"
#include "codeline/engine/ApplianceLink.h"

namespace codeline::engine {

class SignalMast {
public:
  SignalMast(std::string_view name, IMast& mast, bool bind = true) : name_(name), mast_(mast), link_(mast, bind) {}

  SignalMast(const SignalMast&) = delete;
  SignalMast& operator=(const SignalMast&) = delete;

  const std::string& name() const { return name_; }

  SignalIndication demanded() const { return demand_; }
  SignalIndication confirmed() const { return healthy_ ? shown_ : SignalIndication::Stop; }
  // Proof of STOP is a STOP the mast reported after the last command sent to
  // it. A mast reports every command it completes, and nothing while its
  // heads move, so the report it made before a proceed command proves
  // nothing once that command is out (a cancel before the proceed is
  // confirmed leaves the last report at STOP while the heads may show
  // proceed).
  bool provenStop() const { return healthy_ && !awaiting_ && shown_ == SignalIndication::Stop; }
  bool healthy() const { return healthy_; }
  Health applianceHealth() const { return link_.health.health(); }  // informative (debug channel)

  void sample(uint32_t now_ms) {
    const bool healthy = admit(link_.health, mast_, now_ms);
    if (healthy && !healthy_) {  // (re-)admission: state the demand again, and wait for its report
      resend_ = true;
      awaiting_ = true;
    }
    healthy_ = healthy;
    if (healthy) shown_ = mast_.state().indication;
    if (awaiting_ && sent_ && link_.reports != reports_at_send_) awaiting_ = false;
  }

  void demand(SignalIndication i) { demand_ = i; }

  // Egress: one command per change of demand, and once more on (re-)admission
  // so a mast that was lit when its link dropped is told STOP again (S3b).
  void drive() {
    if (!healthy_) return;
    if (!resend_ && demand_ == last_sent_) return;
    reports_at_send_ = link_.reports;
    awaiting_ = true;
    sent_ = true;
    mast_.setIndication(demand_, true);
    last_sent_ = demand_;
    resend_ = false;
  }

private:
  std::string name_;
  IMast& mast_;
  MastLink link_;
  bool healthy_ = false;
  SignalIndication shown_ = SignalIndication::Stop;
  SignalIndication demand_ = SignalIndication::Stop;
  SignalIndication last_sent_ = SignalIndication::Stop;
  bool resend_ = true;
  bool awaiting_ = true;          // no state report yet since the last command (or since admission)
  bool sent_ = false;             // a command has gone out since admission
  uint32_t reports_at_send_ = 0;  // link_.reports when the last command went out
};

}  // namespace codeline::engine
