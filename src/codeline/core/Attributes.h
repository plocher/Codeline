// Attribute metadata and values for Cluster:MAINTENANCE paths
// ("maintenance.offline", "turnout.sim_transit_ms", ...).
//
// The per-appliance tables of AttributeInfo are generated from schema/ by
// tools/gen_appliance_tables.py into src/codeline/generated/.
#pragma once

#include <cstdint>
#include <string_view>

namespace codeline {

// Hundredths: a fixed-point number carried as raw = value * kHundredthsPerUnit (hz).
inline constexpr uint32_t kHundredthsPerUnit = 100;
enum class AttrType : uint8_t { Bool, Uint, Enum, Hundredths };
enum class Persistence : uint8_t { Volatile, NvConfig, NvState };
enum class Access : uint8_t { RO, RW, Vital };

// When a Cluster:MAINTENANCE command is accepted. ByAccess: per attribute,
// RW anytime, VITAL only while OFFLINE.
enum class CommandGate : uint8_t { Anytime, ByAccess, Offline };

struct AttributeInfo {
  const char* path;               // canonical "<cluster>.<attribute>"
  AttrType type;
  Persistence persistence;
  Access access;
  bool clearable;                 // clearAttribute() resets it to its default
  uint32_t default_value;         // raw value, see AttrValue
  uint32_t min_value;             // inclusive
  uint32_t max_value;             // inclusive
  const char* const* enum_names;  // AttrType::Enum only, indexed by value
};

struct AttributeTable {
  const AttributeInfo* items;
  uint8_t count;

  int indexOf(std::string_view path) const {
    for (uint8_t i = 0; i < count; ++i) {
      if (path == items[i].path) return i;
    }
    return -1;
  }
  const AttributeInfo* find(std::string_view path) const {
    const int i = indexOf(path);
    return i < 0 ? nullptr : &items[i];
  }
};

// Every attribute in this slice is a bool, an unsigned integer, a fixed-point
// number, or an enum, so one raw uint32 carries any value. The attribute's AttributeInfo says how
// to read it.
struct AttrValue {
  uint32_t raw = 0;

  static AttrValue boolean(bool b) { return AttrValue{b ? 1u : 0u}; }
  static AttrValue number(uint32_t n) { return AttrValue{n}; }
  template <class E>
  static AttrValue enumerator(E e) { return AttrValue{static_cast<uint32_t>(e)}; }
};

inline bool isValid(const AttributeInfo& info, AttrValue v) { return v.raw >= info.min_value && v.raw <= info.max_value; }

// An AttributeMap / ConfigMap: a bounded list of (attribute, value) pairs.
struct AttrEntry {
  const AttributeInfo* info = nullptr;
  AttrValue value;
};

struct AttrMap {
  static constexpr uint8_t kCapacity = 16;
  AttrEntry entries[kCapacity];
  uint8_t count = 0;

  bool add(const AttributeInfo* info, AttrValue value) {
    if (count == kCapacity) return false;
    entries[count++] = AttrEntry{info, value};
    return true;
  }
  bool get(std::string_view path, AttrValue& out) const {
    for (uint8_t i = 0; i < count; ++i) {
      if (path == entries[i].info->path) {
        out = entries[i].value;
        return true;
      }
    }
    return false;
  }
};

// importConfig() is all-or-nothing: the whole map is applied only if every
// entry names an NV config attribute of `table` and holds a value in range.
// Shared by the cores (which apply it) and the MQTT proxies (which check it
// before encoding, so an out-of-range enum is never spelled).
inline bool importable(const AttributeTable& table, const AttrMap& config) {
  for (uint8_t e = 0; e < config.count; ++e) {
    const AttributeInfo* a = config.entries[e].info ? table.find(config.entries[e].info->path) : nullptr;
    if (!a || a->persistence != Persistence::NvConfig || !isValid(*a, config.entries[e].value)) return false;
  }
  return true;
}

}  // namespace codeline
