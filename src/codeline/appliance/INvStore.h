// Non-volatile storage port for NV config and NV state attributes, keyed by
// attribute path. Every attribute in this slice fits in a uint32.
#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace codeline {

struct INvStore {
  virtual ~INvStore() = default;
  virtual bool load(std::string_view key, uint32_t& value) const = 0;  // false if never saved
  virtual void save(std::string_view key, uint32_t value) = 0;
};

// Fixed-capacity store in RAM. Survives a logical restart (online()) because
// it lives outside the core; a scenario can also plant bad values in it.
class InMemoryNvStore final : public INvStore {
  static constexpr size_t kSlots = 16;
  static constexpr size_t kKeyMax = 47;

  struct Slot {
    char key[kKeyMax + 1];
    uint32_t value;
  };
  Slot slots_[kSlots] = {};
  size_t used_ = 0;

  int find(std::string_view key) const {
    for (size_t i = 0; i < used_; ++i) {
      if (key == slots_[i].key) return static_cast<int>(i);
    }
    return -1;
  }

public:
  bool load(std::string_view key, uint32_t& value) const override {
    const int i = find(key);
    if (i < 0) return false;
    value = slots_[i].value;
    return true;
  }

  void save(std::string_view key, uint32_t value) override {
    int i = find(key);
    if (i < 0) {
      if (used_ == kSlots || key.size() > kKeyMax) return;  // full: the write is lost
      i = static_cast<int>(used_++);
      std::memcpy(slots_[i].key, key.data(), key.size());
      slots_[i].key[key.size()] = '\0';
    }
    slots_[i].value = value;
  }
};

}  // namespace codeline
