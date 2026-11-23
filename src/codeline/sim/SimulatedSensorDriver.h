// In-memory detector behind the ISensorDriver port: a train shunts it or
// leaves it. See docs/bindings/Cpp.md section 5.
#pragma once

#include <cstdint>

#include "codeline/appliance/ISensor.h"

namespace codeline {

class SimulatedSensorDriver final : public ISensorDriver {
public:
  // ---- driver port ----
  bool readDetector() const override { return detected_; }
  void setIdentifyBeacon(bool on) override { beacon_ = on; }

  void tick(uint32_t) {}  // nothing timed; here so every rig ticks drivers alike

  // ---- physics ----
  void shunt(bool detected) { detected_ = detected; }  // a wheelset bridges (or leaves) the circuit

  // ---- observation ----
  bool beacon() const { return beacon_; }

private:
  bool detected_ = false;
  bool beacon_ = false;
};

}  // namespace codeline
