# Smart Appliance Capability Cluster Reference

This document is the normative library of reusable **Capability Clusters** for [**Smart Appliances**](../SmartAppliances.md) and [**Codeline**](../Codeline.md). It pairs with the [**Smart Appliance Catalog**](../appliances/README.Appliances.md), which specifies how concrete railroad appliances compose these clusters.

### Table Legend & Token Syntax
- **`↦`**: Appliance **subscribes** to this MQTT topic for commands.
- **`⇤`**: Appliance **publishes** its own state or events to this MQTT topic.
- **`🅁`**: MQTT topic is **Retained** by the broker and requires `Last Will and Testament` (LWT) handling by the publisher.
- **`TOKEN` / `(TOKEN)`**: Bare `TOKEN` asserts the condition (`true`/`ON`); parenthesized `(TOKEN)` negates it (`false`/`OFF`). Complementary pairs such as `NORMAL (REVERSE)` / `(NORMAL) REVERSE` form **positive control** (neither asserted = unchanged; both asserted = invalid).
- **`RO` / `RW` / `VITAL`**: Attribute write access classes:
  - **`RO`**: Read-only runtime state or hardware metadata.
  - **`RW`**: Read/write anytime (non-vital presentation, calibration palette, or lighting attributes).
  - **`VITAL`**: Read anytime; write requires `OFFLINE` (vital behavior, direct actuator/aspect overrides, or structural `NV config`).

### Design Rules
- **Clusters vs. Appliances**: Each reusable and railroad-specific capability is defined once here as a self-contained **Cluster** with uniform **↦ COMMANDS**, **⇤ EVENTS**, and **ATTRIBUTES** tables (empty sections omitted). Concrete appliances in the [Smart Appliance Catalog](../appliances/README.Appliances.md) declare their composed clusters, timelines, and invariants.
- **Two Planes, Four Topics**:
  - *Operational Plane*: `↦ /cmd` (unretained) and `⇤ /state` (`🅁`, the appliance's sole retained topic; always reports honest physical state).
  - *Management Plane*: `↦ /maint` (`<VERB> <args>`, unretained—plus channel-wide `{channel}/maint` broadcast for discovery) and `⇤ /event` (`<NOUN> <payload>`, unretained).
- **Cluster-Qualified & Sub-Component Attributes**: Every attribute of every composed cluster is addressable via `MAINTENANCE` (`SET`, `CLEAR`, `EXPORT`, `IMPORT`) using canonical `[<component>.]<cluster>.<attribute>` paths—root-level on simple appliances (`position.speed`) and component-qualified on compound appliances (`head.A.position.current`, `head.A.color.palette.YELLOW`, `head.A.RED.onoff.on`). Clusters define no separate `/maint` verbs; all maintenance control uses `MAINTENANCE`'s `SET` (`=`, `+=`, `-=`), `CLEAR`, and `CYCLE`.
- **Dynamic Discovery, LWT, and Vital Fail-Safe Ownership**: Configuration is never retained on the broker. Controllers discover live appliances via `{channel}/maint` `EXPORT CONFIG` and boot-time `/event CONFIG` announcements. Appliances report honest physical state, `/event FAULT`, `/event OFFLINE`, and broker LWT on disconnect; vital fail-safe substitution belongs to the supervisory controller (or `Cluster:APB` on standalone lines) rather than individual generic appliances.
- **Persistence & Boot Defaults**: Every attribute declares its persistence class: `volatile` (runtime state with an explicit boot default), `NV config` (non-volatile provisioned configuration exported/imported via `CONFIG`), or `NV state` (non-volatile runtime counter).
- **Orthogonal Dimensions**: Turning a lighted appliance `OFF` (e.g., approach-lighting `(LIT)`) and back `ON` (`LIT`) preserves its `DIMMING` brightness and `COLOR` palette. Operational turnout commands always use `NORMAL (REVERSE)` / `(NORMAL) REVERSE`, never `POSITION` percentages.
- **No Fake Capabilities**: A fixed-resistor GPIO LED (`MCP23017` / `SMINI`) exposes `ON/OFF` without `DIMMING` or `COLOR`. A Tortoise stall motor or twin-coil solenoid exposes `TURNOUT` without `POSITION`.
- **`FAULT <TYPE>` Taxonomy**: `MAINTENANCE` defines the `/event FAULT <TYPE> [(description)]` envelope and universal fault types. Domain clusters declare any additional fault `<TYPE>` values in their own **⇤ EVENTS** tables and in the registry below.
- **Hardware hygiene is an implementation detail**: Mechanical contact debouncing and input noise filtering reside in hardware or firmware below the Smart Appliance interface.

### `FAULT <TYPE>` Registry

| Fault `<TYPE>` | Declaring Cluster | Meaning |
| :--- | :--- | :--- |
| `HW_ERROR` | [`Cluster:MAINTENANCE`](#clustermaintenance) | Attached peripheral failure (I²C bus, I/O expander, or driver IC) while the MCU remains online. |
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

### ↦ COMMANDS received from controller
Topics: `{appliance}/maint` and broadcast `{channel}/maint` *(both unretained)*

| Command | Behavior |
| :--- | :--- |
| `IDENTIFY ON`<br>`IDENTIFY OFF`<br>`IDENTIFY <ms>` | Activates, deactivates, or pulses physical identification (LED/tone) for `<ms>`.<br>Permitted in both `ONLINE` and `OFFLINE`; must never actuate moving trackside hardware.<br>Broadcast `IDENTIFY OFF` on `{channel}/maint` silences all beacons layout-wide. |
| `EXPORT ATTRIBUTES`<br>`EXPORT CONFIG` | `EXPORT ATTRIBUTES` publishes volatile attributes on `/event ATTRIBUTES`.<br>`EXPORT CONFIG` publishes persisted `NV config` on `/event CONFIG`<br>*(with `0–250 ms` random jitter on `{channel}/maint` broadcast for dynamic discovery)*. |
| `OFFLINE`<br>`ONLINE` | Maintenance mode gate.<br><br>`OFFLINE` sets `maintenance.offline = true`, emits `/event OFFLINE`,<br>and unlocks `VITAL` writes, `IMPORT`, `CYCLE`, and `CLEAR FAULT`<br>while keeping `/cmd` and physical `/state` live for bench testing.<br><br>`ONLINE` triggers a **logical restart** *(see **RESTART BEHAVIOR** below)*. |
| `SET [<comp>.]<cluster>.<attr> [= \| += \| -=] <val>`<br>`CLEAR [<comp>.]<cluster>.<attr>`<br>`CLEAR FAULT` | Sets, nudges (`+=`/`-=`), or clears any writable attribute across composed clusters<br>(`RW` attributes write anytime; **`VITAL`** attributes and `CLEAR FAULT` require `OFFLINE`).<br><br>`CLEAR FAULT` re-verifies the physical condition and clears `maintenance.faulted` only if resolved.<br>`CLEAR` resets runtime counters/flags (`maintenance.fault_count` → `0`, `apb.stick` → `false`)<br>or empties lists (`groups.members` → `[]`). |
| **OFFLINE**<br>`IMPORT <config>` | Bulk `NV config` provisioning via structured cluster-keyed data<br>(equivalent to repeated NV `SET` updates); publishes updated `/event CONFIG`. |
| **OFFLINE**<br>`CYCLE <count> <dwell_ms>` | Exercises the appliance repeatedly for mechanical break-in or lamp/visual inspection. |

### ⇤ EVENTS sent to controller

| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/event` | | `FAULT <TYPE> [(description)]` | Universal fault envelope with optional human-readable detail.<br><br>On any fault except `NON_VITAL_ERROR`, firmware sets `maintenance.faulted = true`,<br>increments `maintenance.fault_count`, and emits `/event FAULT`<br>*(dead MCUs or lost networks are detected via MQTT LWT)*.<br><br>On `NON_VITAL_ERROR`, firmware logs the event and increments `maintenance.fault_count`<br>without setting `maintenance.faulted`.<br>**Declared universal types**: `HW_ERROR`, `CONFIG_ERROR`, `VITAL_ERROR`, `NON_VITAL_ERROR`. |
| `/event` | | `OFFLINE`<br>`ONLINE` | Published whenever `maintenance.offline` transitions. |
| `/event` | | `ATTRIBUTES <data>` | Structured cluster-keyed volatile attributes published in response to<br>`EXPORT ATTRIBUTES` or after volatile `SET` changes. |
| `/event` | | `CONFIG <data>` | Structured cluster-keyed `NV config` published once on boot (hot-plug announcement),<br>after `IMPORT` or `NV config` `SET` changes, or in response to `EXPORT CONFIG`. |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `maintenance.offline` | `boolean`, volatile, RO | `false` (`ONLINE`) | Current operational maintenance gate.<br>Always resets to `false` (`ONLINE`) on reboot. |
| `maintenance.id` | `boolean`, volatile, RO | `false` | Active state of the physical identification beacon. |
| `maintenance.faulted` | `boolean`, volatile, RO | `false` | Set immediately on boot or runtime if hardware/contact verification fails. |
| `maintenance.fault_count` | `number`, NV state, RO | `0` | Increments only on transitions from confirmed healthy to faulted.<br>Reset only via `/maint CLEAR maintenance.fault_count`. |
| `maintenance.post` | `boolean`, NV config, `VITAL` | `true` *(non-moving)*<br>`false` *(`TURNOUT`)* | Briefly exercises attached physical hardware on boot.<br>Defaults `false` for moving actuators that may interfere with objects on the layout (`TURNOUT`). |

### RESTART BEHAVIOR *(Power-On and Exiting `OFFLINE`)*
Both power-on boot and `/maint ONLINE` execute the same initialization sequence (on `ONLINE`, in-place without dropping the MQTT session):
1. Cancel any active `CYCLE` or direct bench overrides, leaving device in a deterministic "end of cycle" state.
2. Reload persisted `NV config` and refresh `groups.members` subscriptions.
3. Reset volatile attributes to their declared boot defaults (`maintenance.offline = false`, `maintenance.id = false`) and re-verify hardware/contacts (`maintenance.faulted`).
4. Run POST if `maintenance.post == true`.
5. Enter the appliance's declared **Startup State** and publish `/state`, `/event CONFIG`, and *(when exiting `OFFLINE`)* `/event ONLINE` last.

---

## `Cluster:GROUPS`
*Optional addressing cluster — decentralized, subscription-based multicast membership modeled on Matter's `Groups` cluster (e.g., dimming an entire CTC panel, identifying all appliances at a control point, or controlling room lighting zones).*

### ↦ COMMANDS received from controller
Topics: `{channel}/group/{group}/maint` for each entry in `groups.members` *(unretained)*

| Command | Behavior |
| :--- | :--- |
| *(Any `MAINTENANCE` command)* | Applies the maintenance or `RW` attribute update (e.g., `IDENTIFY`, `SET dimming.brightness=60`)<br>to every appliance subscribed to `{group}`.<br><br>**Silent ignore**: If a group message targets a cluster or attribute an appliance does not compose<br>(e.g., `dimming.brightness` sent to a station group containing a `TURNOUT`),<br>that appliance silently ignores it without raising a `FAULT`.<br><br>**`VITAL` gate preserved**: Group delivery never bypasses `OFFLINE`;<br>`VITAL` attributes ignore group `SET` unless the appliance is already `OFFLINE`. |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `groups.members` | `list<string>`, NV config, `VITAL` | `[]` | Group slugs this appliance subscribes to (e.g., `["ctc-panel", "corporal"]`).<br>Updated via `SET groups.members+=<slug>` (subscribe & persist),<br>`SET groups.members-=<slug>` (unsubscribe & persist), `SET groups.members=[...]`,<br>or `CLEAR groups.members` (unsubscribe all). |

### LIFECYCLE & MULTI-GROUP RULES
- **Decentralized Membership**: No central group-registry server exists. A controller defines a group by writing its slug into each member's `groups.members` list; each appliance persists the list in NV flash and subscribes to `{channel}/group/{slug}/maint`.
- **Dynamic Reconstruction**: Broadcasting `EXPORT CONFIG` on `{channel}/maint` reconstructs every active group and member directly from live appliances' `groups.members` attributes.
- **Dissolution**: Removing a slug from the last member unsubscribes it and leaves zero retained state on the broker.
- **Last-Write-Wins Across Overlapping Groups**: An appliance in multiple groups (`["ctc-panel", "corporal"]`) applies incoming group commands in arrival order with no group-priority hierarchy.

```text
1. Provision membership (while OFFLINE):
   control/Corporal/maint   ──▶ SET groups.members+=ctc-panel
   control/Sargent/maint    ──▶ SET groups.members+=ctc-panel
   (Each column saves "ctc-panel" to NV flash and subscribes to
    {channel}/group/ctc-panel/maint.)

2. Operate group (live, while ONLINE):
   group/ctc-panel/maint    ──▶ SET dimming.brightness=45
   (Broker fans out in parallel; every CTC column dims to 45% in unison.)

3. Dissolve membership (while OFFLINE):
   control/Corporal/maint   ──▶ SET groups.members-=ctc-panel
   (Column unsubscribes and removes "ctc-panel" from NV flash.)
```

---

## `Cluster:ON/OFF`
*Base indicator and relay cluster — binary active/inactive output orthogonal to `DIMMING` and `COLOR`.*

### ↦ COMMANDS received from controller
| Topic | 🅁 | Command | Behavior |
| :--- | :--- | :--- | :--- |
| `/cmd` | | `ON`<br>`(ON)` | *(Root-level `ON/OFF` appliances such as [`Appliance:LAMP`](../appliances/README.Appliances.md#appliancelamp);<br>omitted on sub-components driven by a parent cluster such as `MAST` or `LEVER`.)*<br>Strictly accepts `ON` or `(ON)` orthogonal to `DIMMING` and `COLOR`;<br>any other payload is rejected. |

### ⇤ EVENTS sent to controller
| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/state` | 🅁 | `ON`<br>`(ON)` | *(Root-level `ON/OFF` appliances such as [`Appliance:LAMP`](../appliances/README.Appliances.md#appliancelamp); omitted on sub-components.)*<br>Initializes to `(ON)` on boot. |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `onoff.on` | `boolean`, volatile, `VITAL` *(wayside)* / `RW` *(`LAMP`)* | `false` | Current binary output state.<br>Turning `false` and back `true` preserves composed `DIMMING` and `COLOR` settings. |

---

## `Cluster:DIMMING`
*Optional cluster — PWM lamps, signal heads, flashers, and panel indicators.*

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `dimming.brightness` | `percent (0..100)`, NV config, `RW` | `100` | Gamma-corrected intensity corresponding to the `B` axis in HSB.<br>Transitions smoothly over `dimming.fade_ms` when changed (supports live `Cluster:GROUPS` dimming). |
| `dimming.fade_ms` | `number`, NV config, `RW` | `0` | Incandescent ramp-up / ramp-down and color-transition time (`ms`). |

---

## `Cluster:COLOR`
*Optional cluster — RGB, RGBA, and addressable pixel indicators and lighting.*

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `color.palette` | `map<name, color>`, NV config, `RW` | AREMA palette | Maps semantic names to calibrated device values (`SET color.palette.YELLOW=(0xFFFF, 0xA800, 0x0000)`):<br>• *Signal aspects*: `RED`, `YELLOW`, `GREEN`, `LUNAR`, `AMBER`, `WHITE`<br>• *Safety & auxiliary*: `BLUE` *(blue-flag / safety lockout)*, `PURPLE` / `PLUM`<br>• *Direct coordinates*: 16-bit RGB tuple `($R, $G, $B)` or HSB tuple `($H, $S, $B)` (with `$B` aligned to `dimming.brightness`). |
| `color.current` | `color`, volatile, `VITAL` *(wayside)* / `RW` *(`LAMP`)* | `RED` *(signals)*<br>`NONE` *(general)* | Active displayed color value (transitions over `dimming.fade_ms` if composed). |

---

## `Cluster:POSITION`
*Optional actuator cluster — servo, stepper, and linear actuators (composed onto servo turnouts, semaphore blades, train-order arms, and crossing gates; omitted on discrete two-state actuators such as Tortoise stall motors and twin-coil solenoids).*

### ⇤ EVENTS sent to controller
| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/event` | | `FAULT OVERCURRENT [(description)]` | Actuator current exceeded configured limit. |
| `/event` | | `FAULT STALL [(description)]` | Actuator jammed mechanically or failed to reach target position. |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `position.current` | `percent (0.00..100.00)`, volatile, `VITAL` | `unknown` | Current normalized position (`0.00%`–`100.00%` of safe travel), reported on `/event ATTRIBUTES`.<br>Supports direct `SET position.current=<pct \| preset>` (e.g., `=45.00` or `=CLEAR`) and incremental `+=` / `-=` nudging. |
| `position.presets` | `map<state, pct>`, NV config, `VITAL` | `{}` | Explicit saved numeric positions per railroad state (`Normal`/`Reverse`, `Stop`/`Approach`/`Clear`, `Up`/`Down`), written explicitly from observed `position.current` (e.g., `SET position.presets.CLEAR=72.50`). |
| `position.motion_profile` | `LINEAR \| EASE`, NV config, `VITAL` | `LINEAR` | Velocity profile during actuator travel. |
| `position.speed` | `pct/sec`, NV config, `VITAL` | `25.0` | Actuator travel speed. |
| `position.idle_power` | `HOLD \| DETACH`, NV config, `VITAL` | `DETACH` | Power state after reaching target (prevents servo stall heating). |
| `position.range_deg` | `number`, NV config (metadata), `RO` | `null` *(optional)* | Physical angular sweep in degrees if known. |

---

## `Cluster:HOLDOFF`
*Required cluster on smart optical and IR track sensors — bridges inter-car wheel gaps locally; omitted on current-transformer detectors (e.g., DCCODs) whose holdoff is implemented in hardware RC timing.*

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `holdoff.make_ms` | `number`, NV config, `VITAL` | `0` | Continuous detection duration (`ms`) before asserting `ACTIVE`.<br>Defaults to `0` for immediate detection; set above `0` (e.g., `50–100`)<br>only when ambient-light sensors need brief shadow filtering. |
| `holdoff.dropout_ms` | `number`, NV config, `VITAL` | `1500` | Continuous clear duration (`ms`) before releasing to `(ACTIVE)`,<br>bridging inter-car coupler gaps locally. |

---

## `Cluster:LEVER`
*Operator I/O cluster — hardware-neutral multi-position switch/lever inputs (`SW1..N`) paired with feedback indicator outputs (`LAMP1..M`).*

### ↦ COMMANDS received from controller
| Topic | 🅁 | Command | Behavior |
| :--- | :--- | :--- | :--- |
| `/cmd` | | Complete ordered frame for `1..lever.outputs`:<br>`LAMP1` \| `(LAMP1)` `...` `LAMP<M>` \| `(LAMP<M>)`| All-or-nothing state vector specifying every output `1..lever.outputs` in order.<br>Missing, extra, out-of-order, or unrecognized tokens reject the entire message.<br>*(e.g., `LAMP1 (LAMP2) LAMP3`)*  |

### ⇤ EVENTS sent to controller
| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/state` | 🅁 | Complete ordered frame for `1..lever.inputs`:<br>`SW1` \| `(SW1)` `...` `SW<N>` \| `(SW<N>)`| Published on boot and whenever any physical lever/contact position changes.<br>Always emits all `1..lever.inputs` in order. <br>*(e.g., `SW1 (SW2)`)* |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `lever.inputs` | `number`, NV config, `VITAL` | `1` | Number of physical switch/lever contact channels (`SW1..SW<N>`). |
| `lever.outputs` | `number`, NV config, `VITAL` | `0` | Number of physical feedback indicator outputs (`LAMP1..LAMP<M>`). |
| `lever.input_style` | `MAINTAINED \| MOMENTARY_2 \|`<br>`MOMENTARY_1 \| ROTARY`,<br>NV config, `VITAL` | `MAINTAINED` | Physical switch/lever mechanism (maintained toggle/quadrant,<br>dual momentary pushbuttons, single toggle pushbutton, or N-position rotary). |

---

## `Cluster:BUTTON`
*Optional operator input cluster — hardware-neutral momentary pushbutton(s) (`BTN1..K`) used alongside `Cluster:LEVER`.*

### ⇤ EVENTS sent to controller
| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/event` | | `BTN<k>`<br>`(BTN<k>)` | Published on button actuation (`OneShot` `BTN<k>` on release,<br>or `BTN<k>` / `(BTN<k>)` pair in `LEVEL` mode). |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `button.inputs` | `number`, NV config, `VITAL` | `1` | Number of momentary pushbutton channels (`BTN1..BTN<K>`). |
| `button.mode` | `ONESHOT_RELEASE \| LEVEL`, NV config, `VITAL` | `ONESHOT_RELEASE` | `ONESHOT_RELEASE`: emits `BTN<k>` once on release.<br>`LEVEL`: emits `BTN<k>` on press and `(BTN<k>)` on release. |

---

# Part II: Railroad Domain Clusters

## `Cluster:TURNOUT`
*Railroad switch machine cluster — closed-loop normal/reverse point command and correspondence feedback.*

### ↦ COMMANDS received from controller
| Topic | 🅁 | Command | Behavior |
| :--- | :--- | :--- | :--- |
| `/cmd` | | `NORMAL (REVERSE)`<br>`(NORMAL) REVERSE`<br>`(NORMAL) (REVERSE)` *(Unchanged)* | Positive-control command to align and lock points.<br>`(NORMAL) (REVERSE)` leaves current alignment unchanged.<br>`NORMAL REVERSE` (both asserted) is rejected as invalid. |

### ⇤ EVENTS sent to controller
| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/state` | 🅁 | `NORMAL (REVERSE)`<br>`(NORMAL) REVERSE`<br>`(NORMAL) (REVERSE)` *(In Transit / Unproven)* | Published on boot, in response to `/cmd`, and whenever physical point correspondence changes (including during local hand-throw).<br>`(NORMAL) (REVERSE)` indicates points are unlocked, moving, stalled, or have contradictory contacts. |
| `/event` | | `FAULT SHORTED_CONTACTS [(description)]` | Both Normal and Reverse point contacts closed simultaneously; `/state` reports `(NORMAL) (REVERSE)` because physical alignment is invalid. |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `turnout.feedback_regime` | `2-CONTACT \| 1-CONTACT \| SIMULATED`, NV config, `VITAL` | `2-CONTACT` | `2-CONTACT`: reads physical Normal and Reverse microswitches.<br>`1-CONTACT`: reads Normal contact; emulates Reverse after `sim_transit_ms`.<br>`SIMULATED`: emulates both contacts after `sim_transit_ms`. |
| `turnout.sim_transit_ms` | `number`, NV config, `VITAL` | `1000` | Simulated transit duration (`ms`) spent in `(NORMAL) (REVERSE)` before asserting target correspondence under `1-CONTACT` or `SIMULATED`. |

---

## `Cluster:SENSOR`
*Binary detection cluster — read-only track circuit, optical detector, or discrete contact state.*

### ⇤ EVENTS sent to controller
| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/state` | 🅁 | `ACTIVE` *(Occupied / Triggered)*<br>`(ACTIVE)` *(Vacant / Clear)* | Published on boot and whenever physical detection changes<br>(after `Cluster:HOLDOFF` if composed). |

---

## `Cluster:MAST`
*Railroad signal mast cluster — rulebook-driven indication translation, multi-head aspect display, and approach lighting.*

### ↦ COMMANDS received from controller
| Topic | 🅁 | Command | Behavior |
| :--- | :--- | :--- | :--- |
| `/cmd` | | `<INDICATION>`<br>`<INDICATION> LIT`<br>`<INDICATION> (LIT)`<br>`LIT`<br>`(LIT)` | Sets the demanded indication and translates it to per-head aspects under `mast.rulebook`:<br>`STOP`, `RESTRICTING`, `DIVERGING_RESTRICTING`, `APPROACH`,<br>`DIVERGING_APPROACH`, `ADVANCE_APPROACH`, `APPROACH_MEDIUM`,<br>`DIVERGING_CLEAR`, `CLEAR`.<br><br>Optional `LIT` / `(LIT)` controls approach lighting orthogonally—allowing vital logic<br>to set or update the indication of a dark mast without lighting it immediately<br>*(defaults to `LIT` when omitted)*. |

### ⇤ EVENTS sent to controller
| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/state` | 🅁 | `<INDICATION>`<br>`<INDICATION> (LIT)` | Same indication word set as `/cmd` *(appending `(LIT)` when dark)*.<br>Published once physical heads (lamps or semaphore blades) have assumed the aspect.<br>Moving semaphore blades or imperfectly displayed heads report `STOP`. |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `mast.rulebook` | `string`, NV config, `VITAL` | `SP-1990` | Named rulebook table (e.g., `SPTCO-1960`, `SPTCO-1969`, `SP-1990`, `GCOR-1995`)<br>mapping Mast Indication + Head Count → per-head aspect. |
| `mast.heads` | `number (1..3)`, NV config, `VITAL` | `1` | Number of signal heads (`head.A`, `head.B`, `head.C`).<br>Each head exposes its own composed clusters<br>(`head.A.position.*`, `head.A.color.*`, `head.A.RED.onoff.*`, `head.A.dimming.*`). |
| `mast.flash_hz` | `number`, NV config, `RW` | `1.0` | Flash cadence (`Hz`, 60 FPM default) for flashing aspects.<br>Flashing masts within an interlocking should phase-lock to a shared plant flasher<br>reference (e.g., wall-clock/NTP or periodic group sync) so adjacent heads stay in unison. |

---

## `Cluster:APB`
*Optional wayside logic cluster — single-track Automatic Permissive Block tumble-down and directional stick (`SR`) logic for autonomous `Appliance:SIGNALMAST` endpoints or standalone MSS-style wayside controllers.*

### ↦ COMMANDS received from controller
| Topic | 🅁 | Command | Behavior |
| :--- | :--- | :--- | :--- |
| *(apb.next_signal)*<br>`.../signalmast/830WAB/apb` | 🅁 | `HOME DISTANT`<br>`HOME (DISTANT)`<br>`(HOME) (DISTANT)` | Subscribes to the outbound `/apb` topic of the signal ahead.<br>`(HOME)` forces `STOP`;<br>`HOME (DISTANT)` allows `APPROACH`;<br>`HOME DISTANT` allows `CLEAR`. |

### ⇤ EVENTS sent to controller
| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/apb` | 🅁 | `HOME DISTANT`<br>`HOME (DISTANT)`<br>`(HOME) (DISTANT)` | Published to the signal in rear.<br>`(HOME) (DISTANT)` is the **vital fail-safe line state**.<br><br>`HOME` asserts when `apb.approach_tc == (ACTIVE)` and<br>when (`HOME_in` is asserted or directional stick `apb.stick` is active).<br>`DISTANT` asserts when governed mast state is `APPROACH` or better.<br><br>**Standalone `OFFLINE` / fault behavior**:<br>Because standalone APB runs without a central controller, while `maintenance.offline == true` or faulted,<br>`Cluster:APB` pauses its autonomous solver and holds outbound `/apb` at `(HOME) (DISTANT)`,<br>tumbling upstream APB signals in rear to `STOP`. On `ONLINE` logical restart, `apb.*` config reloads,<br>`apb.stick` resets to `false`, and autonomous evaluation resumes from `STOP` and `(HOME) (DISTANT)`. |
| `/event` | | `STICK SET`<br>`STICK CLEAR` | Reports APB directional stick (`SR`) transitions. |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `apb.mode` | `ABSOLUTE \| PERMISSIVE`, NV config, `VITAL` | `PERMISSIVE` | Selects absolute home/leaving vs. permissive intermediate behavior. |
| `apb.stick` | `boolean`, volatile, `VITAL` | `false` | Active state of the directional traffic stick (`SR`); resettable during benchwork via `CLEAR apb.stick`. |
| `apb.governs_tc` | `topic`, NV config, `VITAL` | `""` | Track sensor topic immediately in advance of this mast. |
| `apb.approach_tc` | `topic`, NV config, `VITAL` | `""` | Track sensor topic in rear (approach block) of this mast. |
| `apb.next_signal` | `topic`, NV config, `VITAL` | `""` | Inbound `/apb` topic of the next signal ahead. |
| `apb.opposing_mast` | `topic`, NV config, `VITAL` | `""` | Opposing mast state topic at the same insulated joint. |

---

## `Cluster:CROSSING`
*Railroad grade-crossing warning cluster — flashers, bell, and gate control with AREMA Part 3 proof.*

### ↦ COMMANDS received from controller
| Topic | 🅁 | Command | Behavior |
| :--- | :--- | :--- | :--- |
| `/cmd` | | `ACTIVE` *(Activate / Preempt)*<br>`(ACTIVE)` *(Release)* | Manual or interlocking activation / preemption override. |

### ⇤ EVENTS sent to controller
| Topic | 🅁 | Payload | Behavior |
| :--- | :--- | :--- | :--- |
| `/state` | 🅁 | `FLASHING GATES_DOWN` *(Flashing, gates down)*<br>`FLASHING (GATES_DOWN)` *(Flashing, gates lowering)*<br>`(FLASHING) (GATES_DOWN)` *(Clear, gates up)* | Reports physical flasher and gate-down proof. |
| `/event` | | `TEST_KEY` | Maintainer local crossing test key activated. |

### ATTRIBUTES
| Attribute | Type & Persistence | Default | Behavior |
| :--- | :--- | :--- | :--- |
| `crossing.flash_hz` | `number`, NV config, `RW` | `0.85` | Alternating flasher cadence (`Hz`; ~50 FPM per AREMA 45–65 FPM standard). |
| `crossing.gate_delay_ms` | `number`, NV config, `VITAL` | `3000` | Warning flash duration (`ms`) before lowering crossing gates. |
| `crossing.bell_muted` | `boolean`, volatile, `RW` | `false` | Silences crossing bell during benchwork or operating sessions without disabling flashers or gates. |
