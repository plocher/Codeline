// A signal lever's stored demand in the field: the home signal stick (HSR)
// and its direction. Harvested from FieldUnit src/SignalControl.h.
//
// What moved out: the time element (TER) now belongs to the route that was
// cleared (Route::State::Releasing / Timing), so approach and time locking
// start when a route drops for ANY reason, and only once its mast is proven
// at STOP (spike section 4, change 5; section 5, defect 2). Fleeting (FSR)
// is not ported: README.CodeLine section 5.1 rule 2 calls it a future
// extension and the chart has no FL function.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "codeline/engine/Types.h"

namespace codeline::engine {

class SignalLever {
public:
  explicit SignalLever(std::string_view name) : name_(name) {}

  SignalLever(const SignalLever&) = delete;
  SignalLever& operator=(const SignalLever&) = delete;

  const std::string& name() const { return name_; }

  // HSR up in this direction.
  Direction active() const { return active_; }
  // An accepted demand that has not yet cleared a route (README.CodeLine
  // section 5.1 rule 1: held only while switches of its frame settle).
  bool pending() const { return pending_; }

  uint32_t timeLockingMs() const { return time_locking_ms_; }
  void setTimeLockingMs(uint32_t ms) { time_locking_ms_ = ms; }

  // An accepted LEFT or RIGHT: the stick picks (again).
  void demand(Direction d) {
    active_ = d;
    pending_ = d != Direction::None;
  }
  void fulfilled() { pending_ = false; }
  // STOP demand, refusal, train acceptance, or a route dropping: the stick
  // drops and stays down until a new control (no re-clear without Fleeting).
  void drop() {
    active_ = Direction::None;
    pending_ = false;
  }

private:
  std::string name_;
  Direction active_ = Direction::None;  // restart: every signal at STOP
  bool pending_ = false;
  uint32_t time_locking_ms_ = 15000;  // TER default (docs/Codeline.md section 4 step 5)
};

}  // namespace codeline::engine
