// Tick-driven time for Smart Appliance cores. See docs/bindings/Cpp.md,
// "Tick-Driven Elapsed Timers".
//
// Nothing here reads a wall clock. The owner calls TickClock::advanceTo(now_ms)
// once per tick(); production firmware passes millis(), scenarios pass any
// value they like.
#pragma once

#include <cstdint>

namespace codeline {

struct TickClock {
  uint32_t now_ms = 0;
  void advanceTo(uint32_t ms) { now_ms = ms; }
};

// Elapsed-time counter. Correct across uint32_t rollover because all
// arithmetic is modulo 2^32.
class ElapsedMillis {
  const TickClock& clock_;
  uint32_t origin_ms_;

public:
  explicit ElapsedMillis(const TickClock& clk, uint32_t initial_elapsed = 0)
      : clock_(clk), origin_ms_(clk.now_ms - initial_elapsed) {}

  operator uint32_t() const { return clock_.now_ms - origin_ms_; }

  ElapsedMillis& operator=(uint32_t val) {
    origin_ms_ = clock_.now_ms - val;
    return *this;
  }
  ElapsedMillis& operator-=(uint32_t val) {
    origin_ms_ += val;
    return *this;
  }
  ElapsedMillis& operator+=(uint32_t val) {
    origin_ms_ -= val;
    return *this;
  }
};

}  // namespace codeline
