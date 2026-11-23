// In-memory signal heads behind the IMastDriver port, modeled as searchlight
// heads: changing a head's color moves its mechanism, which takes move_ms;
// switching the lamp on or off (NONE, the dark half of a flash) is instant.
// Faults, as a driver sees them:
//   fail(h)      the head never proves and the driver reports it failed
//                (headFailed), e.g. a jammed servo it can detect;
//   stick(h)     the head never proves and the driver cannot tell why: it
//                looks like a move that never ends;
//   killLamp(h)  a dead lamp proven by lamp current, as a color-light head
//                is: NONE proves (no current expected), every color fails and
//                is reported failed.
//
// The port has no clock, so the simulator has its own tick(now_ms). Whoever
// owns time ticks the driver before the core.
#pragma once

#include <cstdint>

#include "codeline/appliance/ISignalMast.h"
#include "codeline/core/TickClock.h"
#include "codeline/generated/Rulebooks.h"

namespace codeline {

class SimulatedMastDriver final : public IMastDriver {
public:
  explicit SimulatedMastDriver(uint32_t move_ms = 300) : move_ms_(move_ms) {}

  // ---- driver port ----

  void setHead(uint8_t head, ColorName color) override {
    if (head >= kMaxHeads) return;
    Head& h = heads_[head];
    h.color = color;
    ++h.set_count;
    if (color == ColorName::None || color == h.mechanism) return;
    h.mechanism = color;
    h.moving = true;
    h.move_start_ms = clock_.now_ms;
  }

  void setLamps(bool lit) override { lit_ = lit; }

  bool headProven(uint8_t head) const override {
    if (head >= kMaxHeads) return false;
    const Head& h = heads_[head];
    return !h.failed && !h.stuck && !h.moving && !(h.lamp_dead && h.color != ColorName::None);
  }

  bool headFailed(uint8_t head) const override {
    if (head >= kMaxHeads) return false;
    const Head& h = heads_[head];
    return h.failed || (h.lamp_dead && h.color != ColorName::None);
  }

  void setIdentifyBeacon(bool on) override { beacon_ = on; }

  // ---- physics ----

  void tick(uint32_t now_ms) {
    clock_.advanceTo(now_ms);
    for (Head& h : heads_) {
      if (h.moving && clock_.now_ms - h.move_start_ms >= move_ms_) h.moving = false;  // modulo 2^32
    }
  }

  // ---- fault injection ----

  void fail(uint8_t head, bool failed) {
    if (head < kMaxHeads) heads_[head].failed = failed;
  }

  void stick(uint8_t head, bool stuck) {
    if (head < kMaxHeads) heads_[head].stuck = stuck;
  }

  void killLamp(uint8_t head, bool dead) {
    if (head < kMaxHeads) heads_[head].lamp_dead = dead;
  }

  // ---- observation ----

  // What a person standing at the mast sees on `head`: NONE when dark.
  ColorName shows(uint8_t head) const { return lit_ ? commanded(head) : ColorName::None; }
  ColorName commanded(uint8_t head) const { return head < kMaxHeads ? heads_[head].color : ColorName::None; }
  bool lit() const { return lit_; }
  bool beacon() const { return beacon_; }
  uint32_t setCount(uint8_t head) const { return head < kMaxHeads ? heads_[head].set_count : 0; }

private:
  struct Head {
    ColorName color = ColorName::None;      // last setHead()
    ColorName mechanism = ColorName::None;  // where the mechanism is (or is going)
    bool moving = false;
    bool failed = false;
    bool lamp_dead = false;
    bool stuck = false;
    uint32_t set_count = 0;
    uint32_t move_start_ms = 0;  // read only while moving
  };

  TickClock clock_;
  uint32_t move_ms_;
  Head heads_[kMaxHeads];
  bool lit_ = false;
  bool beacon_ = false;
};

}  // namespace codeline
