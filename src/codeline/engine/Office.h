// The office side of the engine, shaped by the function model of the Code
// Line Interface (docs/codeline/README.CodeLine.md section 3): a function is
// (class, appliance, word), where the appliance is the bare name inside the
// interlocking ("829", "827", "830", "1MC", "829T1") and, for a signal, the
// signal lever's number, never a mast.
//
// A chart mapper walks the derived chart (tools/codeline_chart.py) and, for
// each entry, calls ControlTransaction::assertFunction(class, appliance,
// word) for an asserted control, or Indications::get(class, appliance, word)
// for an indication. Encodings and chart identity are not the engine's
// business; a transaction that names a function the plant does not have is
// malformed.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace codeline::engine {

enum class FunctionClass : uint8_t { Switch, Lock, Signal, MaintainerCall, TrackCircuit, Lamp };

enum class Word : uint8_t {
  Normal, Reverse,           // switch
  Left, Right, Stop, Time,   // signal
  Release, Released, Request, // lock
  Call,                      // maintainer_call
  Occupied,                  // track_circuit
  Lit,                       // lamp
};

class Plant;

// What the field did with a control transaction (README.CodeLine 5.3).
enum class ControlResult : uint8_t {
  Applied,       // valid and safe: every control applied
  VitalIgnored,  // valid but unsafe: every vital control ignored, non-vital applied
  Malformed,     // an invalid group or an unknown function: ignored as a whole
};

// One station's complete control content. Every function not asserted is
// dropped; a positive-control group with nothing asserted is "unchanged".
class ControlTransaction {
public:
  // Asserts one control function. Naming a function the plant does not
  // have, or a second word of a positive-control group, makes the
  // transaction malformed.
  void assertFunction(FunctionClass cls, std::string_view appliance, Word word);
  bool malformed() const { return malformed_; }

private:
  friend class Plant;
  explicit ControlTransaction(const Plant& plant);

  // Bit sets of asserted words. Switch: 1 NORMAL, 2 REVERSE. Signal: 1 LEFT,
  // 2 RIGHT, 4 STOP.
  static constexpr uint8_t kNormal = 1, kReverse = 2;
  static constexpr uint8_t kLeft = 1, kRight = 2, kStop = 4;

  const Plant* plant_;
  std::vector<uint8_t> switches_;  // per plant switch
  std::vector<uint8_t> signals_;   // per signal lever
  std::vector<uint8_t> locks_;     // per electric lock: RELEASE asserted
  std::vector<uint8_t> calls_;     // per maintainer call: CALL asserted
  bool malformed_ = false;
};

// The plant's complete indication content at one moment.
class Indications {
public:
  // False if the plant has no such indication function.
  bool get(FunctionClass cls, std::string_view appliance, Word word, bool& asserted) const;
  // Asserted, and the function exists.
  bool is(FunctionClass cls, std::string_view appliance, Word word) const {
    bool v = false;
    return get(cls, appliance, word, v) && v;
  }

  bool operator==(const Indications& o) const {
    return switch_n_ == o.switch_n_ && switch_r_ == o.switch_r_ && lock_released_ == o.lock_released_ &&
           lock_request_ == o.lock_request_ && signal_l_ == o.signal_l_ && signal_r_ == o.signal_r_ &&
           signal_t_ == o.signal_t_ && calls_ == o.calls_ && occupied_ == o.occupied_ && lamps_ == o.lamps_;
  }
  bool operator!=(const Indications& o) const { return !(*this == o); }

private:
  friend class Plant;
  explicit Indications(const Plant& plant) : plant_(&plant) {}

  const Plant* plant_;
  std::vector<uint8_t> switch_n_, switch_r_;           // per switch
  std::vector<uint8_t> lock_released_, lock_request_;  // per electric lock
  std::vector<uint8_t> signal_l_, signal_r_, signal_t_;  // per signal lever
  std::vector<uint8_t> calls_;                         // per maintainer call
  std::vector<uint8_t> occupied_;                      // per track circuit
  std::vector<uint8_t> lamps_;                         // per contact, then per unbound lamp
};

}  // namespace codeline::engine
