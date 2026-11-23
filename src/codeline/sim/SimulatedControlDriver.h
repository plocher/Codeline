// In-memory operator surface behind the IControlDriver port: levers and
// buttons a person moves, lamps the core drives. Channel k is bit k-1.
#pragma once

#include <cstdint>

#include "codeline/appliance/IControl.h"

namespace codeline {

class SimulatedControlDriver final : public IControlDriver {
public:
  // ---- driver port ----
  uint32_t readSwitches() const override { return switches_; }
  uint32_t readButtons() const override { return buttons_; }
  void driveLamps(uint32_t bits) override {
    lamps_ = bits;
    ++drive_count_;
  }
  void setIdentifyBeacon(bool on) override { beacon_ = on; }

  void tick(uint32_t) {}  // nothing timed; here so every rig ticks drivers alike

  // ---- manual action ----
  void setSwitch(uint8_t channel, bool closed) { put(switches_, channel, closed); }  // a lever is moved
  void press(uint8_t channel, bool down) { put(buttons_, channel, down); }

  // ---- observation ----
  bool lamp(uint8_t channel) const { return channel >= 1 && channel <= 32 && ((lamps_ >> (channel - 1)) & 1u); }
  uint32_t lamps() const { return lamps_; }
  uint32_t driveCount() const { return drive_count_; }
  bool beacon() const { return beacon_; }

private:
  static void put(uint32_t& bits, uint8_t channel, bool on) {
    if (channel < 1 || channel > 32) return;
    const uint32_t bit = 1u << (channel - 1);
    bits = on ? (bits | bit) : (bits & ~bit);
  }

  uint32_t switches_ = 0;
  uint32_t buttons_ = 0;
  uint32_t lamps_ = 0;
  uint32_t drive_count_ = 0;
  bool beacon_ = false;
};

}  // namespace codeline
