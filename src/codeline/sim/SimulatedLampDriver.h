// In-memory lamp behind the ILampDriver port: the core lights it or puts it
// out. See docs/bindings/Cpp.md section 5.
#pragma once

#include <cstdint>

#include "codeline/appliance/ILamp.h"

namespace codeline {

class SimulatedLampDriver final : public ILampDriver {
public:
  // ---- driver port ----
  void driveLamp(bool on) override {
    lit_ = on;
    ++drive_count_;
  }
  void setIdentifyBeacon(bool on) override { beacon_ = on; }

  void tick(uint32_t) {}  // nothing timed; here so every rig ticks drivers alike

  // ---- observation ----
  bool lit() const { return lit_; }
  uint32_t driveCount() const { return drive_count_; }
  bool beacon() const { return beacon_; }

private:
  bool lit_ = false;
  uint32_t drive_count_ = 0;
  bool beacon_ = false;
};

}  // namespace codeline
