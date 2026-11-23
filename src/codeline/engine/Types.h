// Vital interlocking engine: shared vocabulary.
//
// Harvested from FieldUnit src/types.h, cut to what CP Corporal needs. The
// engine speaks the Smart Appliance Interface's SignalIndication (the
// Cluster:MAST vocabulary) and nothing richer: aspects, heads and rulebooks
// belong to the mast appliance below the seam (docs/research/
// engine-closed-loop-spike.md section 6, items 6 and 10).
#pragma once

#include <cstdint>

#include "codeline/appliance/ISignalMast.h"

namespace codeline::engine {

// What vital logic believes about a switch's points. There is no MOVING the
// engine could invent from its own command (spike section 4, change 2).
enum class Position : uint8_t {
  Unknown,      // never reported since start (unproven)
  Normal,       // appliance reports NORMAL
  Reverse,      // appliance reports REVERSE
  InTransit,    // appliance reports IN_TRANSIT
  Substituted,  // appliance not healthy: fail-safe (NORMAL) (REVERSE)
};

// A signal lever's direction. None is a lever at STOP (HSR down).
enum class Direction : uint8_t { None, Left, Right };

// Names for people (the debug channel, StationHost.h); nothing reads them back.
inline const char* positionName(Position p) {
  switch (p) {
    case Position::Unknown: return "UNKNOWN";
    case Position::Normal: return "NORMAL";
    case Position::Reverse: return "REVERSE";
    case Position::InTransit: return "IN_TRANSIT";
    case Position::Substituted: return "UNHEALTHY";
  }
  return "?";
}
inline const char* directionName(Direction d) {
  switch (d) {
    case Direction::None: return "STOP";
    case Direction::Left: return "LEFT";
    case Direction::Right: return "RIGHT";
  }
  return "?";
}

// Switch lock bits (FieldUnit SwitchLock). A switch may be commanded only
// while it has none (WLR up).
namespace lock {
inline constexpr uint8_t kDetector = 0x01;  // OS track circuit not proven vacant
inline constexpr uint8_t kRoute = 0x02;     // a route is cleared, traversed, or awaiting proof of STOP
inline constexpr uint8_t kTime = 0x04;      // approach / time locking timer (TER) running
}  // namespace lock

// Permissiveness order of the Cluster:MAST indications, least first.
inline uint8_t permissiveness(SignalIndication i) {
  switch (i) {
    case SignalIndication::Stop: return 0;
    case SignalIndication::Restricting:
    case SignalIndication::DivergingRestricting: return 1;
    case SignalIndication::Approach:
    case SignalIndication::DivergingApproach: return 2;
    case SignalIndication::ApproachMedium: return 3;
    case SignalIndication::AdvanceApproach: return 4;
    case SignalIndication::DivergingClear: return 5;
    case SignalIndication::Clear: return 6;
  }
  return 0;
}

inline SignalIndication leastPermissive(SignalIndication a, SignalIndication b) {
  return permissiveness(a) <= permissiveness(b) ? a : b;
}

inline SignalIndication mostPermissive(SignalIndication a, SignalIndication b) {
  return permissiveness(a) >= permissiveness(b) ? a : b;
}

// The DR term released (the next signal is not proven displaying proceed):
// nothing better than an approach-class indication.
inline SignalIndication capAtApproach(SignalIndication i) {
  switch (i) {
    case SignalIndication::Clear:
    case SignalIndication::AdvanceApproach:
    case SignalIndication::ApproachMedium: return SignalIndication::Approach;
    case SignalIndication::DivergingClear: return SignalIndication::DivergingApproach;
    default: return i;
  }
}

}  // namespace codeline::engine
