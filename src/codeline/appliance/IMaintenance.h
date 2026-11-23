// Cluster:MAINTENANCE, C++ binding. See docs/clusters/README.Clusters.md and
// docs/bindings/Cpp.md. Every command is one-way: results arrive later as
// STATE or as an ApplianceEvent.
#pragma once

#include <cstdint>
#include <string_view>

#include "codeline/core/Attributes.h"
#include "codeline/generated/Enums.h"

namespace codeline {

// One discrete event. Pointers and views are valid only during onEvent().
struct ApplianceEvent {
  EventKind kind = EventKind::Fault;
  FaultType fault = FaultType::VitalError;  // kind == Fault
  std::string_view detail;                  // kind == Fault, may be empty
  const AttrMap* data = nullptr;            // kind == Attributes or Config
  uint8_t channel = 0;                      // kind == Button: 1..button.inputs
  bool pressed = false;                     // kind == Button

  static ApplianceEvent of(EventKind k) {
    ApplianceEvent e;
    e.kind = k;
    return e;
  }
};

struct IMaintenance {
  virtual ~IMaintenance() = default;

  // PULSE turns the beacon on for duration_ms; ON and OFF ignore it.
  virtual void identify(IdentifyMode mode, uint32_t duration_ms) = 0;
  virtual void exportAttributes() = 0;
  virtual void exportConfig() = 0;
  virtual void offline() = 0;
  virtual void online() = 0;
  virtual void setAttribute(std::string_view path, SetOp op, AttrValue value) = 0;
  virtual void clearAttribute(std::string_view path) = 0;
  virtual void clearFault() = 0;
  virtual void importConfig(const AttrMap& config) = 0;
  virtual void cycle(uint16_t count, uint32_t dwell_ms) = 0;
};

}  // namespace codeline
