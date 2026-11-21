[![Codeline](../images/codeline-logo.svg)](../../README.md)
[Studio](../Studio.md) · [Codeline](../Codeline.md) · **[Smart Appliances](../SmartAppliances.md)**  
↳ **▸ Appliances** · [Clusters](../clusters/README.Clusters.md) · [C++ Binding](../bindings/Cpp.md) · [MQTT Binding](../bindings/MQTT.md) · *uPython Binding*
---
---

# Smart Appliance Catalog

This document specifies the concrete **Smart Railroad Appliances** for [**Smart Appliances**](../SmartAppliances.md) (see also [**Codeline**](../Codeline.md)). Each appliance is a transport-agnostic logical endpoint composed from the normative capability clusters in the [**Smart Appliance Capability Cluster Reference**](../clusters/README.Clusters.md), organized into **Part I: Wayside Plant Appliances** and **Part II: Operator & Auxiliary Appliances**.

Every appliance exposes typed one-way asynchronous **COMMANDS**, observable physical **STATE** (cached `state()` + `onStateChanged`), discrete **EVENTS** (`onEvent`), canonical `[<component>.]<cluster>.<attribute>` **ATTRIBUTES**, and cooperative `tick(now_ms)` timing. Universal maintenance rules—including the `OFFLINE`/`ONLINE` gate and logical restart sequence—are defined once in [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance); concrete language and wire projections live in companion binding specs such as the [**C++ Binding**](../bindings/Cpp.md) and [**MQTT Binding**](../bindings/MQTT.md).

---

# Part I: Wayside Plant Appliances (Track & Right-of-Way)

## `Appliance:TURNOUT`
*Power, Dual-Control, or Derail Switch Appliance*

### ROLE
`turnout` — power-operated switch machine, dual-control switch, or derail (`829`, `827`, `827D`).

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required; `maintenance.post` defaults to `false`)*
- [`Cluster:TURNOUT`](../clusters/README.Clusters.md#clusterturnout) *(required — `setAlignment(TurnoutCommand)` → `state().alignment: TurnoutState`)*
- [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition) *(optional — for servo, stepper, or linear actuators; omitted for Tortoise stall motors and twin-coil solenoids)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional)*

### BEHAVIOR & INVARIANTS
1. **Startup State**: Reads physical point contacts without energizing the motor (`maintenance.post` defaults to `false`) and emits `state().alignment` (`NORMAL`, `REVERSE`, or `IN_TRANSIT`).
2. **Closed-Loop Transit Sequence**:
   ```text
   Command                        Observed State     Physical Meaning
   ----------------------------   ----------------   -----------------------------------------
   —                              NORMAL             Points locked Normal
   setAlignment(REVERSE)          NORMAL             Reverse command received; motor energized
   —                              IN_TRANSIT         Normal contact opens; points in transit
   —                              REVERSE            Reverse contact closes; points locked Reverse
   ```
3. **Manual Movement & Out-of-Correspondence State**: While points are moving, obstructed, or positioned manually (hand-throw or dual-control in `HAND`), `state().alignment` tracks physical point contacts—reporting `IN_TRANSIT` whenever neither end-of-travel contact is closed. Contradictory contacts emit `FaultEvent(SHORTED_CONTACTS)` while holding `IN_TRANSIT`; actuator faults emit `FaultEvent(STALL)` or `FaultEvent(OVERCURRENT)` via [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition).
4. **Crossovers & Dependent Derails**: Each switch machine or derail in a crossover (`815`, `815A`) or switch-and-derail pair (`827`, `827D`) is an independent `Appliance:TURNOUT` endpoint. For a derail, `NORMAL` is derailing and `REVERSE` is non-derailing.

### MAINTENANCE MODE (`OFFLINE`)
- **`cycle(count, dwell_ms)`**: Alternates physical throws between `NORMAL` and `REVERSE` for `count` cycles, pausing `dwell_ms` at each locked end-stop while reporting live `state().alignment`.
- **Direct Calibration & `VITAL` Writes**: Unlocks `turnout.feedback_regime`, `turnout.sim_transit_ms`, and (when `Cluster:POSITION` is composed) direct actuator positioning (`setAttribute("position.current", ASSIGN | INCREMENT | DECREMENT, ...)`) and preset saves (`setAttribute("position.presets.NORMAL", ASSIGN, pct)` / `REVERSE`).

---

## `Appliance:SENSOR`
*Track Circuit and Binary Contact Detector Appliance*

### ROLE
`sensor` — track occupancy detector (`832T`, `829T1`, `833T`), optical/IR detector, or discrete binary contact (such as a bungalow door monitor).

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:SENSOR`](../clusters/README.Clusters.md#clustersensor) *(required — read-only; `state().active: bool`)*
- [`Cluster:HOLDOFF`](../clusters/README.Clusters.md#clusterholdoff) *(required on optical/IR detectors; omitted on current-transformer detectors with hardware RC holdoff)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional)*

### BEHAVIOR & INVARIANTS
1. **Startup State**: Evaluates physical detection under active `holdoff.*` parameters and emits `state().active` (`true` or `false`).
2. **Read-Only Reporting**: Exposes no operational commands; emits `state().active` whenever filtered physical detection changes.

### MAINTENANCE MODE (`OFFLINE`)
- **`cycle(count, dwell_ms)`**: No-op (read-only input appliance).
- **Direct Calibration & `VITAL` Writes**: Unlocks `holdoff.make_ms`, `holdoff.dropout_ms`, and `groups.members` while `state().active` continues reporting live physical detection for shunt sensitivity and optical alignment testing.

---

## `Appliance:SIGNALMAST`
*Controlled, Absolute, and Permissive Signal Mast Appliance*

### ROLE
`signalmast` — 1-, 2-, or 3-head wayside signal mast (`830WAB`, `831WA`, `828WA`).

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:MAST`](../clusters/README.Clusters.md#clustermast) *(required — `setIndication(SignalIndication, lit)`, `setLit(lit)` → `state(): { indication, lit }`)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional — e.g., station dimming or synchronized flasher group)*
- [`Cluster:APB`](../clusters/README.Clusters.md#clusterapb) *(optional — composed on autonomous `ABSOLUTE` / `PERMISSIVE` masts)*
- **Per-Head Sub-Components (`head.A`, `head.B`, `head.C`)**:
  - *Discrete color-light head*: `head.<X>.<COLOR>` (`RED`, `YELLOW`, `GREEN`, `LUNAR`) each composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) (+ optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))
  - *RGB / searchlight / pixel head*: `head.<X>` composing [`Cluster:COLOR`](../clusters/README.Clusters.md#clustercolor) (+ optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))
  - *Semaphore head*: `head.<X>` composing [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition) (+ optional `head.<X>.lamp` composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) / [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))

### BEHAVIOR & INVARIANTS
1. **Startup State**: Initializes all heads/blades to `STOP` and emits `state() = { indication: STOP, lit: true }` *(with `apb.stick = false` and autonomous evaluation starting from `STOP` and `line = { home: false, distant: false }` if `Cluster:APB` is composed)*.
2. **Transition & Lamp-Failure Reporting**: While a semaphore blade is moving between presets or a required head lamp is faulted, `state().indication` reports `STOP`:
   ```text
   Command                        Observed State     Physical Meaning
   ----------------------------   ----------------   -------------------------------------------
   —                              STOP               Blade at Stop preset
   setIndication(CLEAR, true)     STOP               Actuator moving toward Clear preset
   —                              CLEAR              Actuator reaches Clear preset; reports CLEAR
   ```
3. **Approach Lighting (`lit`)**: `setLit(false)` / `setLit(true)` toggles lamp illumination independently of `indication`, allowing a controller to update a dark mast's indication without illuminating it.
4. **Autonomous APB Operation**: When [`Cluster:APB`](../clusters/README.Clusters.md#clusterapb) is composed, the mast exchanges `ApbLineState(home, distant)` with adjacent signals and tracks directional stick (`apb.stick`) locally.

### MAINTENANCE MODE (`OFFLINE`)
- **`cycle(count, dwell_ms)`**: Steps the mast through each valid indication defined by `mast.rulebook` (or each head's lamps/presets) for `count` passes, holding each step for `dwell_ms`.
- **Direct Sub-Component & `VITAL` Writes**: Unlocks `mast.heads`, `mast.rulebook`, `apb.*`, and direct per-head overrides bypassing rulebook translation—e.g., `setAttribute("head.A.position.current", ASSIGN, CLEAR)` (or `INCREMENT`/`DECREMENT`), `setAttribute("head.A.position.presets.CLEAR", ASSIGN, 7250)`, `setAttribute("head.A.color.current", ASSIGN, YELLOW)`, `setAttribute("head.A.RED.onoff.on", ASSIGN, true)`.
- **Standalone APB Hold**: If `Cluster:APB` is composed, autonomous APB evaluation pauses and holds outbound `line = { home: false, distant: false }` while `OFFLINE`.

---

## `Appliance:TRAINORDER`
*Train Order Signal Face Appliance*

### ROLE
`trainorder` — one directional face (`E`/`W` or `N`/`S`) of a depot train-order board (`Gilroy:W`, `Gilroy:E`). A dual-face board is modeled as two independent 1-head appliances (one per traffic direction).

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:MAST`](../clusters/README.Clusters.md#clustermast) *(required; `mast.heads = 1`, train-order `rulebook` mapped to `STOP` = Form 31, `APPROACH` = Form 19 / Slow, `CLEAR` = No orders)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional)*
- **Sub-Component (`head.A`)**:
  - *Semaphore arm (`UQ3` / `LQ2`)*: `head.A` composing [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition) + optional `head.A.lamp` ([`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) / [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))
  - *Color-light face*: `head.A.<COLOR>` ([`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff)) or `head.A` ([`Cluster:COLOR`](../clusters/README.Clusters.md#clustercolor) + optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))

### BEHAVIOR & INVARIANTS
1. **Startup State**: Initializes `head.A` to `STOP` and emits `state() = { indication: STOP, lit: true }`.

### MAINTENANCE MODE (`OFFLINE`)
- **`cycle(count, dwell_ms)`**: Steps `head.A` through `STOP` → `APPROACH` → `CLEAR` for `count` passes, holding each step for `dwell_ms`.
- **Direct Sub-Component & `VITAL` Writes**: Unlocks `mast.rulebook` and direct `head.A` arm/lamp controls (`head.A.position.current`, `head.A.position.presets.APPROACH`, `head.A.lamp.onoff.on`).

---

## `Appliance:CROSSING`
*Highway Grade Crossing Warning Appliance*

### ROLE
`crossing` — highway grade crossing flashers, bell, and gates driven by `setActive(bool)`.

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:CROSSING`](../clusters/README.Clusters.md#clustercrossing) *(required — `setActive(bool)` → `state(): { flashing, gates_down }`)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional)*
- **Sub-Components**:
  - `flasher` composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) (+ optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))
  - `bell` *(optional)* composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff)
  - `gate.W`, `gate.E` *(optional)* each composing [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition) (presets `UP`, `DOWN`)

### BEHAVIOR & INVARIANTS
1. **Startup State**: Initializes idle (flashers/bell off, gates `UP`) and emits `state() = { flashing: false, gates_down: false }`.
2. **Activation Sequence**: `setActive(true)` energizes flashers and bell (`flashing = true, gates_down = false`), waits `crossing.gate_delay_ms`, lowers gates, and reports `gates_down = true` once gates reach `DOWN`.

### MAINTENANCE MODE (`OFFLINE`)
- **`cycle(count, dwell_ms)`**: Runs `count` complete warning sequences (flashers and bell on, gates lower after `crossing.gate_delay_ms`, hold `dwell_ms`, gates raise, flashers and bell extinguish).
- **Direct Sub-Component & `VITAL` Writes**: Unlocks `crossing.gate_delay_ms` and independent sub-component bench controls (`flasher.onoff.on`, `bell.onoff.on`, `gate.W.position.current`, `gate.W.position.presets.DOWN`) without triggering the full warning sequence (`crossing.flash_hz` and `crossing.bell_muted` remain `RW` anytime).

---

# Part II: Operator & Auxiliary Appliances (Human Control Surfaces & General Lighting)

Part II defines two general-purpose appliances for operator control surfaces, indicators, and layout lighting: **`CONTROL`** (multi-position levers, pushbuttons, and feedback indicators) and **`LAMP`** (binary, dimmable, or color-capable indicators and lighting).

## `Appliance:CONTROL`
*Smart Lever and Feedback Control Appliance*

### ROLE
`control` — multi-channel operator control surface combining switch/lever inputs (`1..N` via [`Cluster:LEVER`](../clusters/README.Clusters.md#clusterlever)), optional momentary pushbuttons (`1..K` via [`Cluster:BUTTON`](../clusters/README.Clusters.md#clusterbutton)), and feedback indicator outputs (`1..M` via `Cluster:LEVER` + per-lamp sub-components).

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:LEVER`](../clusters/README.Clusters.md#clusterlever) *(required — `setLamps(BitVector)` → `state().switches: BitVector`)*
- [`Cluster:BUTTON`](../clusters/README.Clusters.md#clusterbutton) *(optional — `ButtonEvent(channel, pressed)`)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional — e.g., `groups.members = ["ctc-panel"]` for synchronized panel dimming)*
- **Output Sub-Components** *(optional)*: feedback indicator outputs (`lamp.1..M`) each composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) (+ optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming), [`Cluster:COLOR`](../clusters/README.Clusters.md#clustercolor)).

### BEHAVIOR & INVARIANTS
1. **Startup State**: Configures `lever.inputs`, `button.inputs`, and `lever.outputs` from `NV config`, extinguishes `lamp.1..M`, reads physical switch contacts, and emits `state().switches`.

### MAINTENANCE MODE (`OFFLINE`)
- **`cycle(count, dwell_ms)`**: Walks `lamp.1..M` sequentially (or toggles all outputs together) for `count` passes at `dwell_ms` per step to verify panel indicators.
- **Direct Sub-Component & `VITAL` Writes**: Unlocks `lever.inputs`, `lever.outputs`, `lever.input_style`, `button.inputs`, `button.mode`, `groups.members`, and direct per-lamp testing (`setAttribute("lamp.1.onoff.on", ASSIGN, true)`). Physical `state().switches` and `ButtonEvent` remain live for switch and pushbutton continuity testing.

### EXAMPLE USAGE PROFILES

| Example Role | Example `{id}` | Switch Inputs (`lever.inputs`) | Pushbuttons (`button.inputs`) | Lamp Outputs (`lever.outputs`) | Typical Use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Local Switch / Derail Plate** | `Corporal:1`, `827` | `2` (`MAINTAINED` or `MOMENTARY_1`/`2`) | `0` | `2` | Normal/Reverse lever contacts and correspondence lamps |
| **Depot Train-Order Lever** | `Gilroy:W`, `Gilroy:E` | `2` (`MAINTAINED`) | `0` | `0..3` | 3-position order lever (Form 31 / Form 19 / Clear) and repeaters |
| **Electric Switch Lock / Dual-Control** | `lock:827`, `lock:835` | `1` (`MAINTAINED`) | `1` (`LEVEL`) | `1` | Lock/hand status contact, unlock request button, release indicator |
| **Yard Route Selector** | `Watsonville:Arrival` | `N` (`ROTARY`) | `0` | `0..N` | N-position rotary track selector and active-track lamps |
| **CTC Machine Panel Column** | `Corporal` | `N` (`MAINTAINED`) | `1` (`ONESHOT_RELEASE`) | `M` | Station switch/signal/lock levers, CODE button, indication jewels |

*(See [**MQTT Binding: Concrete `CONTROL` Wire Examples**](../bindings/MQTT.md#5-concrete-control-wire-examples) for exact MQTT payloads and [**C++ Binding**](../bindings/Cpp.md) for co-located in-process CTC panel wiring.)*

---

## `Appliance:LAMP`
*General Indicator, Maintainer Call, and Lighting Appliance*

### ROLE
`lamp` — binary, dimmable, or RGB/RGBA indicator, audible output, or lighting fixture (maintainer call lamp/horn, depot and structure lights, or layout room and backdrop lighting).

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) *(required — `setOn(bool)` → `state().on: bool`)*
- [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming) *(optional — PWM brightness and fade ramp)*
- [`Cluster:COLOR`](../clusters/README.Clusters.md#clustercolor) *(optional — calibrated palette, 16-bit `Rgb16`, or `Hsb16`)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional — synchronized room lighting, station, or panel dimming groups)*

### BEHAVIOR & INVARIANTS
1. **Startup State**: Initializes to `on = false` (preserving saved `dimming.*` and `color.palette` settings) and emits `state().on = false`.

### MAINTENANCE MODE (`OFFLINE`)
- **`cycle(count, dwell_ms)`**: Toggles `on` (`true` / `false`)—or sweeps configured palette entries when `Cluster:COLOR` is composed—for `count` passes at `dwell_ms` per step.
- **Direct Calibration & `VITAL` Writes**: Unlocks `groups.members` and `importConfig()`; `onoff.on`, `dimming.*`, and `color.*` remain `RW` in both `ONLINE` and `OFFLINE`.
