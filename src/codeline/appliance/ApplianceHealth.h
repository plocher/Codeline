// Health of one bound appliance, as vital logic needs it for fail-safe
// substitution (docs/Codeline.md "Executive-Owned Vital Fail-Safe
// Substitution"): an appliance that is unproven, UNAVAILABLE, OFFLINE or
// faulted is replaced by its role's fail-safe value.
//
// The same for every appliance type and binding: the controller's observer
// forwards to it, and the controller asks it when to call exportAttributes().
//
//   struct TurnoutBinding final : ITurnoutObserver {
//     ApplianceHealth health;
//     void onStateChanged(TurnoutState) override { health.onState(); }
//     void onEvent(const ApplianceEvent& ev) override { health.onEvent(ev); }
//   };
//   // each scan, before reading state():
//   if (binding.health.attributesDue(now_ms)) turnout.exportAttributes();
//   const bool usable = binding.health.ok();
//
// Why events alone are not enough:
//   - Over MQTT /event is unretained. A controller that connects (or
//     reconnects after its own link or the appliance's link dropped) gets the
//     retained /state but never the OFFLINE or FAULT it missed.
//   - A logical restart re-verifies the hardware and reports any fault that
//     remains *before* its OnlineEvent (RESTART BEHAVIOR steps 3 and 5), and
//     clearFault() emits nothing, so an OnlineEvent alone cannot say whether
//     a fault seen earlier still holds.
// So health is proven only by an AttributesEvent, which carries the
// appliance's own maintenance.offline and maintenance.faulted, and is kept
// current afterwards by FaultEvent / OfflineEvent. It is unproven at start,
// after an OnlineEvent, and after an UnavailableEvent; attributesDue() asks
// for the proof once the appliance has reported state (so it is booted and
// reachable), and again every kRetryMs until it arrives.
//
// Errors in this scheme are restrictive only: anything unknown is unhealthy.
#pragma once

#include <cstdint>

#include "codeline/appliance/IMaintenance.h"

namespace codeline {

enum class Health : uint8_t {
  Ok,           // proven ONLINE and not faulted
  Unproven,     // not (yet, or no longer) proven: start, after ONLINE, after link recovery
  Unavailable,  // the binding lost the appliance (MQTT will or own link loss)
  Offline,      // maintenance.offline
  Faulted,      // maintenance.faulted (a fault other than NON_VITAL_ERROR)
};

class ApplianceHealth {
public:
  static constexpr uint32_t kRetryMs = 1000;  // re-ask for attributes this often while unproven

  void onState() {
    unavailable_ = false;
    reported_ = true;
  }

  void onEvent(const ApplianceEvent& ev) {
    switch (ev.kind) {
      case EventKind::Fault:
        if (ev.fault != FaultType::NonVitalError) faulted_ = true;
        break;
      case EventKind::Offline:
        offline_ = true;
        break;
      case EventKind::Online:
        offline_ = false;
        unprove();  // the restart re-verified; read the result
        break;
      case EventKind::Unavailable:
        unavailable_ = true;
        reported_ = false;
        unprove();
        break;
      case EventKind::Attributes: {
        AttrValue offline, faulted;
        if (ev.data && ev.data->get(kMaintenanceOfflinePath, offline) && ev.data->get(kMaintenanceFaultedPath, faulted)) {
          offline_ = offline.raw != 0;
          faulted_ = faulted.raw != 0;
          proven_ = true;
        }
        break;
      }
      default:
        break;
    }
  }

  // True when the controller should call exportAttributes() now.
  bool attributesDue(uint32_t now_ms) {
    if (proven_ || unavailable_ || !reported_) {
      asked_ = false;
      return false;
    }
    if (asked_ && now_ms - asked_at_ms_ < kRetryMs) return false;
    asked_ = true;
    asked_at_ms_ = now_ms;
    return true;
  }

  Health health() const {
    if (unavailable_) return Health::Unavailable;
    if (!proven_) return Health::Unproven;
    if (offline_) return Health::Offline;
    if (faulted_) return Health::Faulted;
    return Health::Ok;
  }
  bool ok() const { return health() == Health::Ok; }

private:
  bool proven_ = false;       // offline_ / faulted_ came from an AttributesEvent
  bool unavailable_ = false;
  bool reported_ = false;     // state seen since start or since the last UnavailableEvent
  bool offline_ = false;
  bool faulted_ = false;
  bool asked_ = false;
  uint32_t asked_at_ms_ = 0;

  void unprove() {
    proven_ = false;
    asked_ = false;
  }
};

}  // namespace codeline
