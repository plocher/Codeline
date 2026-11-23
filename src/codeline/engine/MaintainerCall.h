// A maintainer call: the one non-vital function of a control point
// (docs/codeline/README.CodeLine.md sections 3 and 5.3). The office's CALL
// control is a level, stored whatever becomes of the frame's vital part;
// the field lights or extinguishes one Appliance:LAMP with it.
//
//   control    CALL asserted: light the lamp; dropped: extinguish it
//   indication CALL asserted only while the lamp itself reports on
//
// The indication is the lamp's report, never the stored control: a lamp that
// is unproven, UNAVAILABLE, OFFLINE or faulted reads not lit (the office is
// not told a maintainer is being called by a lamp nobody can vouch for).
// Nothing vital reads a maintainer call.
//
// Author choices (the docs are silent):
// - The lamp is commanded only while it is healthy, when the stored level
//   changes and once on every (re-)admission, the first included. A lamp
//   restarts dark (Appliance:LAMP Startup State), so a call still stored
//   lights it again when it comes back.
// - A call bound to no lamp (a station file with no lamp for it) has nothing
//   to prove: its indication repeats the stored control.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "codeline/appliance/ILamp.h"
#include "codeline/engine/ApplianceLink.h"

namespace codeline::engine {

class MaintainerCall {
public:
  // Bound to no lamp.
  explicit MaintainerCall(std::string_view name) : name_(name) {}
  MaintainerCall(std::string_view name, ILamp& lamp) : name_(name), lamp_(&lamp), link_(lamp, true) {}

  MaintainerCall(const MaintainerCall&) = delete;
  MaintainerCall& operator=(const MaintainerCall&) = delete;

  const std::string& name() const { return name_; }
  bool bound() const { return lamp_ != nullptr; }

  // The stored control.
  bool called() const { return called_; }
  void call(bool on) { called_ = on; }

  // The indication, as of the last scan.
  bool lit() const { return lamp_ == nullptr ? called_ : lit_; }

  // Informative (debug channel).
  bool healthy() const { return lamp_ == nullptr || healthy_; }
  Health applianceHealth() const { return lamp_ == nullptr ? Health::Ok : link_.health.health(); }

  void sample(uint32_t now_ms) {
    if (lamp_ == nullptr) return;
    const bool healthy = admit(link_.health, *lamp_, now_ms);
    if (healthy && !healthy_) resend_ = true;  // (re-)admission: state the lamp again
    healthy_ = healthy;
    lit_ = healthy_ && lamp_->state();
  }

  void drive() {
    if (lamp_ == nullptr || !healthy_) return;
    if (!resend_ && called_ == last_sent_) return;
    lamp_->setOn(called_);
    last_sent_ = called_;
    resend_ = false;
  }

private:
  // A link that observes nothing, for a call bound to no lamp.
  struct Link final : ILampObserver {
    Link() = default;
    Link(ILamp& l, bool bind) {
      if (bind) l.setObserver(this);
    }
    ApplianceHealth health;
    void onStateChanged(bool) override { health.onState(); }
    void onEvent(const ApplianceEvent& ev) override { health.onEvent(ev); }
  };

  std::string name_;
  ILamp* lamp_ = nullptr;
  Link link_;
  bool called_ = false;  // restart: not called
  bool healthy_ = false;
  bool lit_ = false;
  bool last_sent_ = false;
  bool resend_ = false;
};

}  // namespace codeline::engine
