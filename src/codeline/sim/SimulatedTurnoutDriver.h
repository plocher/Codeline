// In-memory switch machine behind the ITurnoutDriver port: timed point
// travel plus fault-injection controls for scenarios. See docs/bindings/Cpp.md
// section 3 and section 5 ("Bottom-Side Physics & Fault Injection").
//
// The port has no clock, so the simulator has its own tick(now_ms). Whoever
// owns time ticks the driver before the core.
#pragma once

#include <cstdint>

#include "codeline/appliance/ITurnout.h"
#include "codeline/core/TickClock.h"

namespace codeline {

class SimulatedTurnoutDriver final : public ITurnoutDriver {
public:
  enum class Points : uint8_t { Normal, Reverse, Between };

  explicit SimulatedTurnoutDriver(uint32_t travel_ms = 2000, Points start = Points::Normal)
      : travel_ms_(travel_ms), points_(start) {}

  // ---- driver port ----

  // Energizing toward the far end lifts the points off their contact at once;
  // they reach the other end travel_ms later. Unchanged stops them where they are.
  void driveMotor(TurnoutCommand dir) override {
    motor_ = dir;
    ++drive_count_;
    if (dir == TurnoutCommand::Unchanged) {
      travelling_ = false;
      return;
    }
    if (points_ == endFor(dir)) return;
    points_ = Points::Between;
    travelling_ = true;
    travel_timer_ = 0;
  }

  TurnoutContacts readContacts() const override {
    if (forced_) return forced_contacts_;
    return TurnoutContacts{points_ == Points::Normal, points_ == Points::Reverse};
  }

  void setIdentifyBeacon(bool on) override { beacon_ = on; }

  // ---- physics ----

  void tick(uint32_t now_ms) {
    clock_.advanceTo(now_ms);
    if (travelling_ && !jammed_ && travel_timer_ >= travel_ms_) {
      points_ = endFor(motor_);
      travelling_ = false;
    }
  }

  // ---- fault injection & manual action ----

  void handThrow(Points p) {
    points_ = p;
    travelling_ = false;
  }
  void forceContacts(bool normal, bool reverse) {
    forced_ = true;
    forced_contacts_ = TurnoutContacts{normal, reverse};
  }
  void shortContacts() { forceContacts(true, true); }
  void releaseContacts() { forced_ = false; }
  void jam(bool jammed) { jammed_ = jammed; }  // moving points stop between the ends

  // ---- observation ----

  Points points() const { return points_; }
  TurnoutCommand motor() const { return motor_; }
  uint32_t driveCount() const { return drive_count_; }
  bool beacon() const { return beacon_; }

private:
  static Points endFor(TurnoutCommand c) {
    return c == TurnoutCommand::Normal ? Points::Normal : Points::Reverse;
  }

  TickClock clock_;
  ElapsedMillis travel_timer_{clock_};
  uint32_t travel_ms_;
  Points points_;
  bool travelling_ = false;
  bool jammed_ = false;
  bool forced_ = false;
  TurnoutContacts forced_contacts_{false, false};
  TurnoutCommand motor_ = TurnoutCommand::Unchanged;
  uint32_t drive_count_ = 0;
  bool beacon_ = false;
};

}  // namespace codeline
