// Appliance:SIGNALMAST, C++ binding: the typed logical API on top (IMast),
// the observer it reports through, and the narrow driver port underneath
// (IMastDriver). Same shape as ITurnout.h. See docs/bindings/Cpp.md section 3,
// docs/clusters/README.Clusters.md (MAST) and
// docs/appliances/README.Appliances.md (Appliance:SIGNALMAST).
//
// The interface is named IMast, as docs/bindings/Cpp.md and
// docs/SmartAppliances.md name it: it is the typed API of Cluster:MAST plus
// MAINTENANCE, which Appliance:TRAINORDER composes too.
//
// Choices where the docs are silent or short (most restrictive reading):
//
// 1. What state() reports while moving (owner ruling, README.Clusters.md
//    Cluster:MAST STATE `indication`). state() is the last confirmed state.
//    While heads move, the prior confirmed state is held and no new state is
//    emitted, for every change (to STOP, to a proceed, between proceeds,
//    lit/dark). A head the driver reports failed raises FaultEvent HW_ERROR;
//    the mast never emits STOP for a failure. Before the first confirmation
//    after power-on, state() is { STOP, lit = true }, the declared Startup
//    State, but it has not been reported to any observer and is not proof.
//
// 2. Proof of STOP. A STOP the appliance reports was proven on every head.
//    But state() alone cannot tell that report from an unreported startup
//    value, a proxy's fallback after link loss or a bad /state, or a mast
//    that is faulted: vital logic takes proof of STOP only from
//    state().indication == STOP *and* ApplianceHealth::ok()
//    (codeline/appliance/ApplianceHealth.h).
//
// 3. Controller loss. The docs define no behavior for a mast that loses its
//    controller, and this API adds none: a mast keeps displaying its last
//    indication, including a proceed, for as long as it is cut off. Vital
//    logic must therefore hold every lock that depends on this mast being at
//    STOP until the mast is healthy again and reports STOP (spike S3b).
//
// 4. Vocabulary. Only the nine SignalIndication words of Cluster:MAST exist.
//    A controller whose own logic has more (the donor engine's MEDIUM_*,
//    SLOW_*, APPROACH_SLOW, ...) maps each to one of these nine that is no
//    more permissive. The enum's order is the docs' listing order; it is not
//    a permissiveness ranking, so do not compare SignalIndication values with
//    < or >.
#pragma once

#include <cstdint>

#include "codeline/appliance/IMaintenance.h"
#include "codeline/generated/Enums.h"

namespace codeline {

// Cluster:MAST STATE: { indication, lit }.
struct MastState {
  SignalIndication indication = SignalIndication::Stop;
  bool lit = true;  // approach lighting: true = illuminated

  bool operator==(const MastState& o) const { return indication == o.indication && lit == o.lit; }
  bool operator!=(const MastState& o) const { return !(*this == o); }
};

struct IMastObserver {
  virtual ~IMastObserver() = default;
  virtual void onStateChanged(MastState state) = 0;
  virtual void onEvent(const ApplianceEvent& ev) = 0;
};

// Identical for the local core and for a remote appliance behind MQTT.
struct IMast : public IMaintenance {
  // Demands `indication` and sets approach lighting. The docs give `lit` a
  // default of true; C++ default arguments on virtual functions bind to the
  // static type, so callers pass it explicitly.
  virtual void setIndication(SignalIndication indication, bool lit) = 0;
  // Approach lighting alone; the demanded indication is unchanged.
  virtual void setLit(bool lit) = 0;
  virtual MastState state() const = 0;  // last confirmed physical state
  virtual void setObserver(IMastObserver* o) = 0;
  virtual void tick(uint32_t now_ms) = 0;
};

// Provisional: the port MastCore (M2 phase 2) needs. The core owns rulebook
// translation (mast.rulebook, mast.heads) and flash timing (mast.flash_hz);
// the driver only shows colors and proves them.
struct IMastDriver {
  virtual ~IMastDriver() = default;
  // head 0 = head.A, 1 = head.B, 2 = head.C. ColorName::None = the head
  // shows nothing (no aspect, or the dark half of a flash).
  virtual void setHead(uint8_t head, ColorName color) = 0;
  // Approach lighting for every head. A semaphore blade still moves while dark.
  virtual void setLamps(bool lit) = 0;
  // True once the head physically shows the last setHead() color: a blade at
  // its preset, a lamp proven (while dark: proven intact).
  // A driver may prove NONE trivially (no lamp current expected), so the core
  // takes proof of a flashing head only from its lit half: it reads
  // headProven() while the head shows the color, holds that reading through
  // the dark half, and treats a newly shown flashing aspect as unproven until
  // its first lit half proves it.
  virtual bool headProven(uint8_t head) const = 0;
  // True when the head cannot show its aspect and the driver knows it will
  // not recover by itself (lamp out, servo jammed, proving circuit open), as
  // opposed to "not yet proven" (still moving or animating). The core raises
  // HW_ERROR on it. A driver that cannot tell leaves it false; the core then
  // holds the prior confirmed state for as long as the head does not prove.
  // Read like headProven() for flashing heads (lit half only).
  virtual bool headFailed(uint8_t /*head*/) const { return false; }
  virtual void setIdentifyBeacon(bool on) = 0;
};

}  // namespace codeline
