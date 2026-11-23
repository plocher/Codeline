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
  bool provenStop() const { return healthy_ && shown_ == SignalIndication::Stop; }
  bool healthy() const { return healthy_; }

  void sample(uint32_t now_ms) {
    const bool healthy = admit(link_.health, mast_, now_ms);
    if (healthy && !healthy_) resend_ = true;  // (re-)admission: state the demand again
    healthy_ = healthy;
    if (healthy) shown_ = mast_.state().indication;
  }

  void demand(SignalIndication i) { demand_ = i; }

  // Egress: one command per change of demand, and once more on (re-)admission
  // so a mast that was lit when its link dropped is told STOP again (S3b).
  void drive() {
    if (!healthy_) return;
    if (!resend_ && demand_ == last_sent_) return;
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
};

}  // namespace codeline::engine
