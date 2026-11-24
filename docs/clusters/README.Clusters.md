[![Codeline](../images/codeline-logo.svg)](../../README.md)

[Studio](../Studio.md) · [Codeline](../Codeline.md) · **[Smart Appliances](../SmartAppliances.md)**  
↳ [Appliances](../appliances/README.Appliances.md) · **▸ Clusters** · [C++ Binding](../bindings/Cpp.md) · [MQTT Binding](../bindings/MQTT.md) · [Building](../BuildingAppliances.md) · *uPython Binding*

---

# Smart Appliance Capability Cluster Reference

This document is the normative, **transport-agnostic** library of reusable **Capability Clusters** for [**Smart Appliances**](../SmartAppliances.md) and [**Codeline**](../Codeline.md). It pairs with the [**Smart Appliance Catalog**](../appliances/README.Appliances.md), which defines how concrete railroad appliances compose these clusters.

This reference defines only the **Typed Async Logical API** (types, commands, observable physical state, discrete events, attributes, and lifecycle invariants). Mechanical projections onto concrete in-process languages (starting with C++, open to runtimes such as MicroPython) and network transports live in companion binding specs:

- [**C++ In-Process Binding, Reference Core & Test Harness**](../bindings/Cpp.md) — embedded no-exceptions, zero-heap-churn C++ interfaces, tick-driven `ElapsedMillis` timers, Reference Appliance Core + Driver Port, optional mirror glue, and deterministic functional testing.
- [**MQTT Transport & Text Wire Encoding Binding**](../bindings/MQTT.md) — two-plane/four-topic layout, `TOKEN`/`(TOKEN)` positive-control serialization, sole retained state, dynamic broadcast discovery, and broker LWT mapping.

---

## Interaction Model & Canonical Type System

### Pure Asynchronous Interaction Model

Every cluster follows the same non-blocking, asynchronous interaction model so local in-process callers and networked callers share 100% identical semantics:

- **COMMANDS** *(Controller → Appliance, one-way `void` intent)*: Typed operations expressing intent. Commands never return synchronous RPC status codes; physical completion and management updates always arrive asynchronously through **STATE** and **EVENTS**. Invalid operational values are unrepresentable in strongly typed bindings and rejected at wire codec boundaries; `VITAL` writes sent while `ONLINE` are silently ignored.
- **STATE** *(Appliance → Controller, observable physical truth)*: The appliance's current physical condition, exposed both as a non-blocking cached getter (`state()`, initialized unproven/restrictive until first confirmation) for deterministic evaluation ticks and as a push notification (`onStateChanged`) for observers and transport adapters.
- **EVENTS** *(Appliance → Controller, discrete transient notifications)*: Edge-triggered lifecycle, fault, operator-button, and management notifications delivered via `onEvent`. Transport-level disconnects surface as a logical `UNAVAILABLE` event/condition from the active binding.
- **ATTRIBUTES** *(Cluster-qualified runtime and configuration properties)*: Addressable through [`Cluster:MAINTENANCE`](#clustermaintenance) using canonical `[<component>.]<cluster>.<attribute>` paths—root-level on simple appliances (`position.speed`) and component-qualified on compound appliances (`head.A.position.current`, `head.A.color.palette.YELLOW`, `head.A.RED.onoff.on`).
- **Cooperative Lifecycle (`tick(now_ms)`)**: Every appliance implementation provides a non-blocking `tick(uint32 now_ms)` entry point to advance rollover-safe elapsed timers (motion ramps, holdoff timers, simulated transit delays, flasher cadences, `CYCLE` sweeps, and `IDENTIFY` timeouts) deterministically—without requiring an RTOS thread per appliance or coupling logic directly to a platform wall clock.

### Canonical Types & Access Classes

| Category | Logical Type | Semantics |
| :--- | :--- | :--- |
| **Primitives** | `bool`, `uint8`, `uint16`, `uint32`, `int32`, `float32` | Fixed-width embedded-safe scalars. |
| **Domain Scalars** | `percent` (`uint16` `0..10000` hundredths, i.e., `0.00%`–`100.00%`), `duration_ms` (`uint32`), `hz` (`float32`), `string` *(bounded)*, `endpoint_ref` *(opaque handle)* | Exact units and bounded representations for embedded targets; `endpoint_ref` resolves to a direct in-process object reference (e.g., C++ pointer/reference or Python object) or an endpoint address in networked bindings. |
| **Composites** | `BitVector` *(fixed-width bitmask + channel `count`, `0..32` channels)*, `ColorValue` *(tagged `ColorName` \| `Rgb16(r,g,b)` \| `Hsb16(h,s,b)`)*, `list<T>`, `map<K, V>` | Bounded value structures designed for zero/low heap churn in embedded runtimes; `BitVector` represents atomic all-or-nothing multi-channel frames (`SW1..N`, `LAMP1..M`, maximum `32` channels). |
| **Mutation Ops** | `SetOp`: `ASSIGN` \| `INCREMENT` \| `DECREMENT` | Typed attribute mutation operator (enabling incremental actuator nudging and group membership add/remove without ad-hoc string parsing). |
| **Domain Enums** | `TurnoutCommand` (`NORMAL`, `REVERSE`, `UNCHANGED`), `TurnoutState` (`NORMAL`, `REVERSE`, `IN_TRANSIT`), `SignalIndication` (`STOP` .. `CLEAR`), `FaultType`, `FeedbackRegime`, `MotionProfile`, `IdlePower`, `InputStyle`, `ButtonMode`, `ApbMode`, `IdentifyMode` (`ON`, `OFF`, `PULSE`) | Closed enumerations preventing invalid operational states by construction. |
| **Persistence** | `volatile`, `NV config`, `NV state` | `volatile`: runtime state reset to boot default on restart. `NV config`: provisioned configuration exported/imported via `CONFIG`. `NV state`: persisted runtime counter. |
| **Access Classes** | `RO`, `RW`, `VITAL` | `RO`: read-only. `RW`: writable anytime (`ONLINE` or `OFFLINE`). `VITAL`: read anytime; write requires `maintenance.offline == true`. |

### Design Rules

- **Clusters vs. Appliances**: Each capability is defined once here as a self-contained **Cluster** with uniform **COMMANDS**, **STATE**, **EVENTS**, and **ATTRIBUTES** tables (empty sections omitted). Concrete appliances in the [Smart Appliance Catalog](../appliances/README.Appliances.md) declare their composed clusters, startup states, and timelines.
- **Positive Control & Atomic Frames**: Complementary pairs (`TurnoutCommand`, `TurnoutState`, `ApbLineState`) enforce explicit positive-control semantics (neither asserted = unchanged/unproven; both asserted = invalid/fault). Multi-channel operator frames (`Cluster:LEVER`) are always applied and reported as complete, atomic `BitVector` frames.
- **Unified Attribute Maintenance**: Clusters define no separate management verbs; all attribute writes, nudges, clears, and bulk provisioning flow through `Cluster:MAINTENANCE`.
- **Honest Physical State & Vital Fail-Safe Ownership**: Appliances report honest physical state, `OFFLINE`, and `FAULT`, while the active transport binding reports `UNAVAILABLE` on link loss. Vital fail-safe substitution belongs to the supervisory controller (or `Cluster:APB` on standalone lines), never to individual generic appliances.
- **Orthogonal Dimensions & No Fake Capabilities**: Disabling illumination (e.g., `lit = false` or `on = false`) preserves saved `DIMMING` and `COLOR` attributes. Operational turnout commands always use `TurnoutCommand`, never `POSITION` percentages. Fixed-resistor GPIO LEDs compose `ON/OFF` without `DIMMING` or `COLOR`; Tortoise stall motors compose `TURNOUT` without `POSITION`.
- **Hardware Hygiene Below the Seam**: Contact debouncing, pin polarity (Active-Low), and wiring multiplexers (`3MUX2A`) live in hardware or driver bindings below the Smart Appliance API.

### `FAULT <TYPE>` Registry

`Cluster:MAINTENANCE` defines the `FaultEvent(type: FaultType, detail?: string)` envelope and universal fault types. Domain clusters declare additional `FaultType` values in their own **EVENTS** tables and in the registry below.

| `FaultType` | Declaring Cluster | Meaning |
| :--- | :--- | :--- |
| `HW_ERROR` | [`Cluster:MAINTENANCE`](#clustermaintenance) | Attached peripheral failure (I²C bus, I/O expander, or driver IC) while the controller remains online. |
| `CONFIG_ERROR` | [`Cluster:MAINTENANCE`](#clustermaintenance) | Corrupt, missing, or invalid `NV config`. |
| `OVERCURRENT` | [`Cluster:POSITION`](#clusterposition) | Actuator current exceeded its configured limit. |
| `STALL` | [`Cluster:POSITION`](#clusterposition) | Actuator jammed mechanically or failed to reach target position. |
| `SHORTED_CONTACTS` | [`Cluster:TURNOUT`](#clusterturnout) | Both Normal and Reverse point contacts closed simultaneously. |
| `VITAL_ERROR` | [`Cluster:MAINTENANCE`](#clustermaintenance) | Catch-all for uncategorized faults that prevent safe operation. |
| `NON_VITAL_ERROR` | [`Cluster:MAINTENANCE`](#clustermaintenance) | Diagnostic fault that does not compromise safe operation (e.g., enclosure over-temperature, low auxiliary rail). |

---

# Part I: General Capability Clusters

## `Cluster:MAINTENANCE`
*Universal cluster — composed by all Smart Appliances.*

### COMMANDS

| Command Signature | Gate | Behavior |
| :--- | :--- | :--- |
| `identify(mode: IdentifyMode, duration_ms?: duration_ms)` | Anytime | Activates (`ON`), deactivates (`OFF`), or pulses (`PULSE`, `duration_ms`) physical identification (LED/tone). Must never actuate moving trackside hardware. Broadcast `identify(OFF)` silences all beacons layout-wide. |
| `exportAttributes()` | Anytime | Emits `AttributesEvent(data)` carrying all volatile runtime attributes across composed clusters. |
| `exportConfig()` | Anytime | Emits `ConfigEvent(data)` carrying all persisted `NV config` attributes across composed clusters (used for live inventory discovery). |
| `offline()` | Anytime | Sets `maintenance.offline = true`, emits `OfflineEvent`, and unlocks `VITAL` writes, `importConfig`, `cycle`, and `clearFault` while keeping operational commands and physical state reporting live for bench testing. |
| `online()` | Anytime | Triggers an in-place **logical restart** *(see **RESTART BEHAVIOR** below)*. |
| `setAttribute(path: AttrPath, op: SetOp, value: AttrValue)` | `RW`: Anytime<br>`VITAL`: `OFFLINE` | Assigns (`ASSIGN`) or nudges (`INCREMENT` / `DECREMENT`) any writable attribute across composed clusters (`RO` targets and `VITAL` writes while `ONLINE` are ignored). Emits `AttributesEvent` on volatile changes or `ConfigEvent` on `NV config` changes. |
| `clearAttribute(path: AttrPath)` | `RW`: Anytime<br>`VITAL`: `OFFLINE` | Resets writable runtime counters/flags (`maintenance.fault_count` → `0`, `apb.stick` → `false`) or empties collection attributes (`groups.members` → `[]`). |
| `clearFault()` | `OFFLINE` | Re-verifies the physical hardware/contact condition and clears `maintenance.faulted` only if resolved. Emits no event; controllers learn the cleared fault state via `exportAttributes()` (typically during `online()` re-admission). |
| `importConfig(config: ConfigMap)` | `OFFLINE` | Bulk `NV config` provisioning via structured cluster-keyed data. Validated **atomically** on every binding (direct and networked): if any entry is unknown, non-`NV config`, or out of range, the entire import is rejected with no partial writes and no `ConfigEvent`. On success, persists all values and emits `ConfigEvent`. |
| `cycle(count: uint16, dwell_ms: duration_ms)` | `OFFLINE` | Exercises the appliance repeatedly for mechanical break-in or lamp/visual inspection, pausing `dwell_ms` at each step. |

### EVENTS

| Event | Payload | Behavior |
| :--- | :--- | :--- |
| `FaultEvent` | `type: FaultType, detail?: string` | Universal fault notification.<br>On any fault except `NON_VITAL_ERROR`, firmware sets `maintenance.faulted = true`, increments `maintenance.fault_count`, and emits `FaultEvent`.<br>On `NON_VITAL_ERROR`, firmware logs the event and increments `maintenance.fault_count` without setting `maintenance.faulted`.<br>**Non-blocking**: Faults latch `maintenance.faulted = true` so supervisory controllers apply vital fail-safe substitution, but do **not** block operational commands (allowing a maintainer to back points off an obstruction after `STALL` unless a hardware driver inhibits actuation).<br>**Declared universal types**: `HW_ERROR`, `CONFIG_ERROR`, `VITAL_ERROR`, `NON_VITAL_ERROR`. |
| `OfflineEvent` / `OnlineEvent` | *(none)* | Emitted whenever `maintenance.offline` transitions (`false` → `true` emits `OfflineEvent`; logical restart completion emits `OnlineEvent`). |
| `UnavailableEvent` | *(none)* | Transport-level loss notification surfaced by networked bindings when a remote endpoint disconnects unexpectedly. |
| `AttributesEvent` | `data: AttributeMap` | Structured cluster-keyed volatile attributes emitted in response to `exportAttributes()` or after volatile `setAttribute()` changes. |
| `ConfigEvent` | `data: ConfigMap` | Structured cluster-keyed `NV config` emitted once on boot (hot-plug announcement), after `importConfig()` or `NV config` `setAttribute()` changes, or in response to `exportConfig()`. |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `maintenance.offline` | `bool`, volatile, `RO` | `false` (`ONLINE`) | Current operational maintenance gate. Always resets to `false` (`ONLINE`) on reboot. |
| `maintenance.id` | `bool`, volatile, `RO` | `false` | Active state of the physical identification beacon. |
| `maintenance.faulted` | `bool`, volatile, `RO` | `false` | Set immediately on boot or runtime if hardware/contact verification fails. |
| `maintenance.fault_count` | `uint32`, NV state, `RO` *(clearable)* | `0` | Increments only on transitions from confirmed healthy to faulted. Reset only via `clearAttribute("maintenance.fault_count")`. |
| `maintenance.post` | `bool`, NV config, `VITAL` | `true` *(non-moving)*<br>`false` *(`TURNOUT`)* | Briefly exercises attached physical hardware on boot. Defaults `false` for moving actuators that may interfere with objects on the layout (`TURNOUT`). |
| `maintenance.node` | `string`, volatile, `RO` | `""` | *(Post-M3; omitted from M2 schema until shared-socket nodes arrive.)* Runtime identifier of the hosting microcontroller/gateway node when multiple appliances share one networked transport session (empty `""` for in-process or dedicated per-appliance sessions). Reported in `AttributesEvent` so networked bindings can apply node-level LWT without member lists. |

### RESTART BEHAVIOR *(Power-On and Exiting `OFFLINE`)*
Both power-on boot and `online()` execute the same initialization sequence (on `online()`, in-place without tearing down the active transport session):
1. Cancel any active `cycle()` or direct bench overrides, leaving device in a deterministic "end of cycle" state.
2. Reload persisted `NV config` and refresh `groups.members` multicast registrations.
3. Reset volatile attributes to their declared boot defaults (`maintenance.offline = false`, `maintenance.id = false`) and re-verify hardware/contacts (`maintenance.faulted`).
4. Run POST if `maintenance.post == true`.
5. Enter the appliance's declared **Startup State**, emit initial **STATE**, `ConfigEvent`, and *(when exiting `OFFLINE`)* `OnlineEvent` last. *(For an appliance whose startup state requires physical completion before confirming—such as a `SIGNALMAST` moving heads back to `STOP`—`ConfigEvent` and `OnlineEvent` emit immediately and initial **STATE** emits as soon as the heads reach `STOP`.)*

#### Controller Health Proof & Re-Admission
Because `FaultEvent`, `OfflineEvent`, and `OnlineEvent` are transient (unretained on networked bindings), `clearFault()` emits no event, and step 3 re-verifies faults *before* `OnlineEvent` in step 5, **STATE** and discrete events alone cannot prove that an appliance is both `ONLINE` and unfaulted. A supervisory controller therefore treats every appliance's health as **unproven** (applying vital fail-safe substitution) at controller startup, after `UnavailableEvent` recovery (including controller transport reconnect or a shared host node going `UNAVAILABLE`), and after `OnlineEvent`, until it calls `exportAttributes()` and receives an `AttributesEvent` confirming `maintenance.offline == false` and `maintenance.faulted == false`. Once proven healthy, live `OfflineEvent`, `FaultEvent` (other than `NON_VITAL_ERROR`), and `UnavailableEvent` drop health immediately; any missed or delayed `AttributesEvent` errs restrictively.

---

## `Cluster:GROUPS`
*Optional addressing cluster — decentralized multicast membership modeled on Matter's `Groups` cluster (e.g., dimming an entire CTC panel, identifying all appliances at a control point, or controlling room lighting zones).*

### COMMANDS *(Curated Multicast Only — No Attribute Writes)*

To prevent broadcast/group commands from overwriting per-device calibration (`dimming.fade_ms`, `color.palette`, `mast.flash_hz`) or triggering hazardous vital state changes (`offline()`, `cycle()`, `importConfig()`), multicast accepts **only** a curated, domain-namespaced command set with an optional `group` filter (`group = none` targets all appliances; `group = slug` filters to appliances whose `groups.members` contains `slug`):

| Command Signature | Gate | Behavior |
| :--- | :--- | :--- |
| `exportConfig(group?: string)` | Anytime | Matching live appliances emit `ConfigEvent(data)` *(networked bindings apply `0–250 ms` jitter)*. |
| `identify(mode: IdentifyMode, duration_ms?: duration_ms, group?: string)` | Anytime | Activates (`ON`), silences (`OFF`), or pulses (`PULSE`, `duration_ms`) physical identification beacons across matching appliances. |
| `light(action: LightGroupAction, group?: string)` | Operational (`ONLINE`) | Domain-namespaced lighting fan-out across matching appliances:<br>• `DIM(brightness: percent)`: Smoothly sets brightness on any `DIMMING`-capable member (CTC panels, lamps, dimmable heads) using each device's own `dimming.fade_ms`.<br>• `ON` / `OFF`: Toggles `Appliance:LAMP` fixtures *(never extinguishes wayside signal masts or CTC indication jewels)*.<br>• `COLOR(value: ColorValue)`: Sets active color on `COLOR`-capable `Appliance:LAMP` fixtures *(never overrides wayside signal aspects or per-device `color.palette`)*.<br><br>**Silent ignore**: Appliances that do not compose the target domain or capability silently ignore the command without raising a `FaultEvent`. The domain namespace (`LIGHT`, etc.) remains extensible for future curated multicast commands. |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `groups.members` | `list<string>`, NV config, `VITAL` | `[]` | Group slugs this appliance belongs to (e.g., `["ctc-panel", "corporal"]`). Updated point-to-point via `setAttribute("groups.members", INCREMENT, slug)` (join & persist), `DECREMENT` (leave & persist), `ASSIGN` (replace list), or `clearAttribute("groups.members")` (leave all). |

### LIFECYCLE & MULTI-GROUP RULES
- **Decentralized Membership**: No central group-registry server exists. A controller defines a group by adding its slug to each member's `groups.members` list; each appliance persists membership in `NV config` and matches incoming multicast commands locally.
- **Dynamic Reconstruction**: Invoking layout-wide `exportConfig()` across live appliances reconstructs every active group and member directly from their `groups.members` attributes.
- **Dissolution**: Removing a slug from the last member leaves zero stale group state.
- **Last-Write-Wins Across Overlapping Groups**: An appliance in multiple groups (`["ctc-panel", "corporal"]`) applies incoming group commands in arrival order with no group-priority hierarchy.

---

## `Cluster:ON/OFF`
*Base indicator and relay cluster — binary active/inactive output orthogonal to `DIMMING` and `COLOR`.*

### COMMANDS

| Command Signature | Gate | Behavior |
| :--- | :--- | :--- |
| `setOn(on: bool)` | Operational | *(Root-level `ON/OFF` appliances such as [`Appliance:LAMP`](../appliances/README.Appliances.md#appliancelamp); omitted on sub-components driven by a parent cluster such as `MAST` or `LEVER`.)* Sets binary output orthogonally to `DIMMING` and `COLOR`. |

### STATE

| State Field | Type | Behavior |
| :--- | :--- | :--- |
| `on` | `bool` | *(Root-level `ON/OFF` appliances such as [`Appliance:LAMP`](../appliances/README.Appliances.md#appliancelamp); omitted on sub-components.)* Initializes to `false` on boot. |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `onoff.on` | `bool`, volatile, `VITAL` *(wayside)* / `RW` *(`LAMP`)* | `false` | Current binary output state. Turning `false` and back `true` preserves composed `DIMMING` and `COLOR` settings. |

---

## `Cluster:DIMMING`
*Optional cluster — PWM lamps, signal heads, flashers, and panel indicators.*

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `dimming.brightness` | `percent` (`0..10000`), NV config, `RW` | `10000` (`100.00%`) | Gamma-corrected intensity corresponding to the `B` axis in HSB. Transitions smoothly over `dimming.fade_ms` when changed (supports live `Cluster:GROUPS` dimming). |
| `dimming.fade_ms` | `duration_ms`, NV config, `RW` | `0` | Incandescent ramp-up / ramp-down and color-transition time (`ms`). |

---

## `Cluster:COLOR`
*Optional cluster — RGB, RGBA, and addressable pixel indicators and lighting.*

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `color.palette` | `map<ColorName, ColorValue>`, NV config, `RW` | AREMA palette | Maps semantic `ColorName` entries to calibrated device `ColorValue` tuples:<br>• *Signal aspects*: `RED`, `YELLOW`, `GREEN`, `LUNAR`, `AMBER`, `WHITE`<br>• *Safety & auxiliary*: `BLUE` *(blue-flag / safety lockout)*, `PURPLE` / `PLUM`<br>• *Direct coordinates*: 16-bit `Rgb16(r, g, b)` or `Hsb16(h, s, b)` (with `b` aligned to `dimming.brightness`). |
| `color.current` | `ColorValue`, volatile, `VITAL` *(wayside)* / `RW` *(`LAMP`)* | `RED` *(signals)*<br>`NONE` *(general)* | Active displayed color value (transitions over `dimming.fade_ms` if composed). |

---

## `Cluster:POSITION`
*Optional actuator cluster — servo, stepper, and linear actuators (composed onto servo turnouts, semaphore blades, train-order arms, and crossing gates; omitted on discrete two-state actuators such as Tortoise stall motors and twin-coil solenoids).*

### EVENTS

| Event | Payload | Behavior |
| :--- | :--- | :--- |
| `FaultEvent` | `type: OVERCURRENT, detail?: string` | Actuator current exceeded its configured limit. |
| `FaultEvent` | `type: STALL, detail?: string` | Actuator jammed mechanically or failed to reach target position. |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `position.current` | `optional<percent>` (`0..10000`), volatile, `VITAL` | `none` *(unknown)* | Current normalized position (`0.00%`–`100.00%` of safe travel), reported via `AttributesEvent`. Supports direct assignment (percentage or preset name) and `INCREMENT`/`DECREMENT` nudging via `MAINTENANCE::setAttribute`. |
| `position.presets` | `map<PresetName, percent>`, NV config, `VITAL` | `{}` | Explicit saved numeric positions per railroad state (`NORMAL`/`REVERSE`, `STOP`/`APPROACH`/`CLEAR`, `UP`/`DOWN`), written explicitly from observed `position.current`. |
| `position.motion_profile` | `MotionProfile` (`LINEAR` \| `EASE`), NV config, `VITAL` | `LINEAR` | Velocity profile during actuator travel. |
| `position.speed` | `float32` *(pct/sec)*, NV config, `VITAL` | `25.0` | Actuator travel speed in percent per second. |
| `position.idle_power` | `IdlePower` (`HOLD` \| `DETACH`), NV config, `VITAL` | `DETACH` | Power state after reaching target (prevents servo stall heating). |
| `position.range_deg` | `optional<float32>`, NV config (metadata), `RO` | `none` | Physical angular sweep in degrees if known. |

---

## `Cluster:HOLDOFF`
*Required cluster on smart optical and IR track sensors — bridges inter-car wheel gaps locally; omitted on current-transformer detectors (e.g., DCCODs) whose holdoff is implemented in hardware RC timing.*

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `holdoff.make_ms` | `duration_ms`, NV config, `VITAL` | `0` | Continuous detection duration (`ms`) before asserting `active = true`. Defaults to `0` for immediate detection; set above `0` (e.g., `50–100`) only when ambient-light sensors need brief shadow filtering. |
| `holdoff.dropout_ms` | `duration_ms`, NV config, `VITAL` | `1500` | Continuous clear duration (`ms`) before releasing to `active = false`, bridging inter-car coupler gaps locally. |

---

## `Cluster:LEVER`
*Operator I/O cluster — hardware-neutral multi-position switch/lever inputs (`1..lever.inputs`) paired with feedback indicator outputs (`1..lever.outputs`).*

### COMMANDS

| Command Signature | Gate | Behavior |
| :--- | :--- | :--- |
| `setLamps(lamps: BitVector)` | Operational | Atomic all-or-nothing state vector specifying every feedback output `1..lever.outputs`. If `lamps.count != lever.outputs`, the entire command is rejected with no partial output change. *(One-way output with no lamp-confirmation channel; electric-lock release withdrawal relies on two-step re-lock-then-clear transactions or an optional controller-side withdrawal dwell.)* |

### STATE

| State Field | Type | Behavior |
| :--- | :--- | :--- |
| `switches` | `BitVector` *(length `lever.inputs`)* | Complete ordered physical switch/lever contact vector `1..lever.inputs`. Emitted on boot and whenever any physical lever/contact position changes. |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `lever.inputs` | `uint8` (`1..32`), NV config, `VITAL` | `1` | Number of physical switch/lever contact channels (`1..32`, bounded by `BitVector` width). |
| `lever.outputs` | `uint8` (`0..32`), NV config, `VITAL` | `0` | Number of physical feedback indicator outputs (`0..32`, bounded by `BitVector` width). |
| `lever.input_style` | `InputStyle` (`MAINTAINED` \| `MOMENTARY_2` \| `MOMENTARY_1` \| `ROTARY`), NV config, `VITAL` | `MAINTAINED` | Physical switch/lever mechanism (maintained toggle/quadrant, dual momentary pushbuttons, single toggle pushbutton, or N-position rotary). |

---

## `Cluster:BUTTON`
*Optional operator input cluster — hardware-neutral momentary pushbutton(s) (`1..button.inputs`) used alongside `Cluster:LEVER`.*

### EVENTS

| Event | Payload | Behavior |
| :--- | :--- | :--- |
| `ButtonEvent` | `channel: uint8` (`1..K`), `pressed: bool` | Emitted on button actuation (`ONESHOT_RELEASE`: emits `pressed = true` once on release; `LEVEL`: emits `pressed = true` on press and `pressed = false` on release). |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `button.inputs` | `uint8` (`0..32`), NV config, `VITAL` | `1` | Number of momentary pushbutton channels (`0..32`, bounded by `BitVector` width; `0` disables buttons when `Cluster:BUTTON` is statically composed on `CONTROL`). |
| `button.mode` | `ButtonMode` (`ONESHOT_RELEASE` \| `LEVEL`), NV config, `VITAL` | `ONESHOT_RELEASE` | `ONESHOT_RELEASE`: emits once on release. `LEVEL`: emits on both press and release edges. |

---

# Part II: Railroad Domain Clusters

## `Cluster:TURNOUT`
*Railroad switch machine cluster — closed-loop normal/reverse point command and correspondence feedback.*

### COMMANDS

| Command Signature | Gate | Behavior |
| :--- | :--- | :--- |
| `setAlignment(cmd: TurnoutCommand)` | Operational | Positive-control command to align and lock points (`NORMAL`, `REVERSE`, or `UNCHANGED`). `UNCHANGED` leaves current alignment untouched. Bindings carrying separate Normal/Reverse flags must reject contradictory `(normal=true, reverse=true)` inputs as invalid. |

### STATE

| State Field | Type | Behavior |
| :--- | :--- | :--- |
| `alignment` | `TurnoutState` (`NORMAL` \| `REVERSE` \| `IN_TRANSIT`) | Honest physical point correspondence emitted on boot, during commanded motion, and whenever physical point contacts change (including local hand-throw). `IN_TRANSIT` indicates points are unlocked, moving, stalled, or have contradictory contacts. |

### EVENTS

| Event | Payload | Behavior |
| :--- | :--- | :--- |
| `FaultEvent` | `type: SHORTED_CONTACTS, detail?: string` | Both Normal and Reverse point contacts closed simultaneously; `state().alignment` reports `IN_TRANSIT` because physical alignment is invalid. |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `turnout.feedback_regime` | `FeedbackRegime` (`TWO_CONTACT` \| `ONE_CONTACT` \| `SIMULATED`), NV config, `VITAL` | `TWO_CONTACT` | `TWO_CONTACT`: reads physical Normal and Reverse microswitches. `ONE_CONTACT`: reads Normal contact; emulates Reverse after `sim_transit_ms`. `SIMULATED`: emulates both contacts after `sim_transit_ms`. |
| `turnout.sim_transit_ms` | `duration_ms`, NV config, `VITAL` | `1000` | Simulated transit duration (`ms`) spent in `IN_TRANSIT` before asserting target correspondence under `ONE_CONTACT` or `SIMULATED`. |

*(Note: `Cluster:TURNOUT` does not time physical throws or emit `STALL`—motor drive characteristics such as continuous Tortoise stall holding vs. solenoid pulses belong to the hardware driver or [`Cluster:POSITION`](#clusterposition). Points that fail to arrive remain `IN_TRANSIT`, which vital logic treats as out of correspondence.)*

---

## `Cluster:SENSOR`
*Binary detection cluster — read-only track circuit, optical detector, or discrete contact state.*

### STATE

| State Field | Type | Behavior |
| :--- | :--- | :--- |
| `active` | `bool` (`true` = Occupied / Triggered, `false` = Vacant / Clear) | Read-only physical detection state emitted on boot and whenever filtered detection changes (after `Cluster:HOLDOFF` if composed). |

---

## `Cluster:MAST`
*Railroad signal mast cluster — rulebook-driven indication translation, multi-head aspect display, and approach lighting.*

### COMMANDS

| Command Signature | Gate | Behavior |
| :--- | :--- | :--- |
| `setIndication(indication: SignalIndication, lit: bool = true)` | Operational | Sets the demanded `SignalIndication` (`STOP`, `RESTRICTING`, `DIVERGING_RESTRICTING`, `APPROACH`, `DIVERGING_APPROACH`, `ADVANCE_APPROACH`, `APPROACH_MEDIUM`, `DIVERGING_CLEAR`, `CLEAR`), translates it to per-head aspects under `mast.rulebook`, and applies approach-lighting state `lit`. |
| `setLit(lit: bool)` | Operational | Controls approach lighting orthogonally without changing the active `SignalIndication`—allowing vital logic to extinguish or illuminate a mast independently of its solved indication. |

### STATE

| State Field | Type | Behavior |
| :--- | :--- | :--- |
| `indication` | `SignalIndication` | Honest physical indication emitted **after every completed command** once all heads (lamps or semaphore blades) have finished moving to the commanded aspect—even if the completed indication equals the prior reported value (so a cancelled mid-motion move still emits a fresh `STOP` report upon settling). While heads are moving, hold the prior confirmed `indication` and do **not** emit a state update until movement completes. Consequently, a supervisory controller counts a mast as proven at `STOP` **only on a `STOP` state report received after the `STOP` command**, never from the last report the mast happened to send earlier. Hardware lamp-out / head-proof sensing is **optional** and defaults to assumed-proven ("never faults") on simple LED heads; drivers that monitor head health raise a `FaultEvent` on an unrecoverable head failure rather than emitting `STOP`. *(No controller-loss watchdog is defined on `Cluster:MAST`; if a mast disconnects, vital logic holds locks indefinitely until the mast recovers and confirms `STOP`.)* |
| `lit` | `bool` | Current approach-lighting illumination state (`true` = illuminated, `false` = dark). |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `mast.rulebook` | `string`, NV config, `VITAL` | `"SP-1990"` | Named rulebook table (e.g., `SPTCO-1960`, `SPTCO-1969`, `SP-1990`, `GCOR-1995`) mapping `(SignalIndication, mast.heads)` → per-head aspects. |
| `mast.heads` | `uint8` (`1..3`), NV config, `VITAL` | `1` | Number of signal heads (`head.A`, `head.B`, `head.C`). Each head exposes its own composed clusters (`head.A.position.*`, `head.A.color.*`, `head.A.RED.onoff.*`, `head.A.dimming.*`). |
| `mast.flash_hz` | `hz`, NV config, `RW` | `1.0` | Flash cadence (`Hz`, 60 FPM default) for flashing aspects. Flashing masts within an interlocking should phase-lock to a shared plant flasher reference (e.g., wall-clock/NTP or periodic group sync) so adjacent heads stay in unison. |

### `SignalIndication` Permissiveness Ranking
The 9 values below form the normative Smart Appliance `SignalIndication` vocabulary (engines with richer internal indications map them to a value **no more permissive**). Vital route evaluation (least-permissive along a route, most-permissive across routes) and `Cluster:APB` `distant` (`APPROACH` or better) use this explicit permissiveness ranking—**never** enum declaration order:

| Rank | `SignalIndication` Value(s) | `DISTANT` (`APPROACH` or better) |
| :-: | :--- | :-: |
| `0` | `STOP` | No |
| `1` | `RESTRICTING`, `DIVERGING_RESTRICTING` | No |
| `2` | `APPROACH`, `DIVERGING_APPROACH` | Yes |
| `3` | `APPROACH_MEDIUM` | Yes |
| `4` | `ADVANCE_APPROACH` | Yes |
| `5` | `DIVERGING_CLEAR` | Yes |
| `6` | `CLEAR` | Yes |

*(**`DARK` is not a `SignalIndication`.** When a layout project file lists `DARK` in a head's `presents` array, it denotes approach-lighting capability—represented orthogonally as `lit = false` via `setLit(false)` or `setIndication(indication, false)` so an extinguished mast retains its underlying solved indication and vane/blade position.)*

---

## `Cluster:APB`
*Optional wayside logic cluster — single-track Automatic Permissive Block tumble-down and directional stick (`SR`) logic for autonomous `Appliance:SIGNALMAST` endpoints or standalone MSS-style wayside controllers.*

### COMMANDS

| Command Signature | Gate | Behavior |
| :--- | :--- | :--- |
| `onNextSignalChanged(line: ApbLineState)` | Operational | Receives outbound `ApbLineState(home, distant)` from `apb.next_signal`. `home = false` forces `STOP`; `(home = true, distant = false)` allows `APPROACH`; `(home = true, distant = true)` allows `CLEAR`. |

### STATE

| State Field | Type | Behavior |
| :--- | :--- | :--- |
| `line` | `ApbLineState` (`{ home: bool, distant: bool }`) | Outbound line state emitted to the signal in rear. `(home = false, distant = false)` is the **vital fail-safe line state**.<br><br>`home` asserts when `apb.approach_tc` is clear (`active == false`) and (`next_signal.home` is asserted or directional stick `apb.stick` is active).<br>`distant` asserts when governed mast `state().indication` is `APPROACH` or better (rank `≥ 2` in the [`SignalIndication` Permissiveness Ranking](#signalindication-permissiveness-ranking)).<br><br>**Standalone `OFFLINE` / fault behavior**: Because standalone APB runs without a central controller, while `maintenance.offline == true` or `maintenance.faulted == true`, `Cluster:APB` pauses its autonomous solver and holds outbound `line` at `(home = false, distant = false)`, tumbling upstream APB signals in rear to `STOP`. On `online()` logical restart, `apb.*` config reloads, `apb.stick` resets to `false`, and autonomous evaluation resumes from `STOP` and `(home = false, distant = false)`. |

### EVENTS

| Event | Payload | Behavior |
| :--- | :--- | :--- |
| `StickEvent` | `set: bool` | Emitted whenever APB directional stick (`SR`) transitions (`true` = set, `false` = cleared). |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `apb.mode` | `ApbMode` (`ABSOLUTE` \| `PERMISSIVE`), NV config, `VITAL` | `PERMISSIVE` | Selects absolute home/leaving vs. permissive intermediate behavior. |
| `apb.stick` | `bool`, volatile, `VITAL` | `false` | Active state of the directional traffic stick (`SR`); resettable during benchwork via `clearAttribute("apb.stick")`. |
| `apb.governs_tc` | `endpoint_ref`, NV config, `VITAL` | `null` | Reference to the `SENSOR` endpoint immediately in advance of this mast. |
| `apb.approach_tc` | `endpoint_ref`, NV config, `VITAL` | `null` | Reference to the `SENSOR` endpoint in rear (approach block) of this mast. |
| `apb.next_signal` | `endpoint_ref`, NV config, `VITAL` | `null` | Reference to the `APB` peer endpoint of the next signal ahead. |
| `apb.opposing_mast` | `endpoint_ref`, NV config, `VITAL` | `null` | Reference to the opposing `SIGNALMAST` endpoint at the same insulated joint. |

---

## `Cluster:CROSSING`
*Railroad grade-crossing warning cluster — flashers, bell, and gate control with AREMA Part 3 proof.*

### COMMANDS

| Command Signature | Gate | Behavior |
| :--- | :--- | :--- |
| `setActive(active: bool)` | Operational | Manual or interlocking activation / preemption override (`true` = activate warning, `false` = release). |

### STATE

| State Field | Type | Behavior |
| :--- | :--- | :--- |
| `flashing` | `bool` | Physical flasher operation proof. |
| `gates_down` | `bool` | Physical crossing gate-down proof (`(flashing=true, gates_down=false)` while gates are lowering; `(flashing=false, gates_down=false)` when clear and gates up). |

### EVENTS

| Event | Payload | Behavior |
| :--- | :--- | :--- |
| `TestKeyEvent` | *(none)* | Maintainer local crossing test key activated. |

### ATTRIBUTES

| Attribute | Type, Persistence & Access | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `crossing.flash_hz` | `hz`, NV config, `RW` | `0.85` | Alternating flasher cadence (`Hz`; ~50 FPM per AREMA 45–65 FPM standard). |
| `crossing.gate_delay_ms` | `duration_ms`, NV config, `VITAL` | `3000` | Warning flash duration (`ms`) before lowering crossing gates. |
| `crossing.bell_muted` | `bool`, volatile, `RW` | `false` | Silences crossing bell during benchwork or operating sessions without disabling flashers or gates. |
