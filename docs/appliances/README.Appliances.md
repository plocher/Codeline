# Smart Appliance Catalog

This document specifies the concrete **Smart Railroad Appliances** for [**Smart Appliances**](../SmartAppliances.md) (see also [**Codeline**](../Codeline.md)). Each appliance is an MQTT endpoint composed from the normative capability clusters in the [**Smart Appliance Capability Cluster Reference**](../clusters/README.Clusters.md), organized into **Part I: Wayside Plant Appliances** and **Part II: Operator & Auxiliary Appliances**.

Every appliance implements the **two-plane, four-topic model** (`↦ /cmd`, `⇤ 🅁 /state`, `↦ /maint` + `{channel}/maint`, `⇤ /event`) and exposes all composed cluster attributes under canonical `[<component>.]<cluster>.<attribute>` paths. Universal maintenance rules—including the `OFFLINE`/`ONLINE` gate and logical restart sequence—are defined once in [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance); each appliance entry below specifies only its appliance-specific startup state, operational behavior, and `OFFLINE` tools.

---

# Part I: Wayside Plant Appliances (Track & Right-of-Way)

## `Appliance:TURNOUT`
*Power, Dual-Control, or Derail Switch Appliance*

### ROLE
`turnout` — power-operated switch machine, dual-control switch, or derail (`829`, `827`, `827D`)

### TOPIC
`{channel}/turnout/{id}`

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required; `maintenance.post` defaults to `false`)*
- [`Cluster:TURNOUT`](../clusters/README.Clusters.md#clusterturnout) *(required — `/cmd`: `NORMAL (REVERSE)` / `(NORMAL) REVERSE`; `/state`: `NORMAL (REVERSE)` / `(NORMAL) REVERSE` / `(NORMAL) (REVERSE)`)*
- [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition) *(optional — for servo, stepper, or linear actuators; omitted for Tortoise stall motors and twin-coil solenoids)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional)*

### BEHAVIOR & INVARIANTS
1. **Startup State**: Reads physical point contacts without energizing the motor (`maintenance.post` defaults to `false`; `/cmd` is unretained) and publishes `/state` (`NORMAL (REVERSE)`, `(NORMAL) REVERSE`, or `(NORMAL) (REVERSE)`).
2. **Closed-Loop Transit Sequence**:
   ```text
   /cmd (↦)            /state (⇤ 🅁)          Physical Meaning
   -----------------   -------------------   -----------------------------------------
   ----                NORMAL (REVERSE)      Points locked Normal
   (NORMAL) REVERSE    NORMAL (REVERSE)      Reverse command received; motor energized
   ----                (NORMAL) (REVERSE)    Normal contact opens; points in transit
   ----                (NORMAL) REVERSE      Reverse contact closes; points locked Reverse
   ```
3. **Manual Movement & Out-of-Correspondence State**: While points are moving, obstructed, or positioned manually (hand-throw or dual-control in `HAND`), `/state` tracks the physical point contacts—reporting `(NORMAL) (REVERSE)` whenever neither end-of-travel contact is closed. Hardware stall and overcurrent events are reported via [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition) (`FAULT STALL`, `FAULT OVERCURRENT`).
4. **Crossovers & Dependent Derails**: Each switch machine or derail in a crossover (`815`, `815A`) or switch-and-derail pair (`827`, `827D`) is an independent `Appliance:TURNOUT` endpoint. For a derail, `NORMAL (REVERSE)` is derailing and `(NORMAL) REVERSE` is non-derailing.

### MAINTENANCE MODE (`OFFLINE`)
- **`CYCLE <count> <dwell_ms>`**: Alternates physical throws between `NORMAL` and `REVERSE` for `<count>` cycles, pausing `<dwell_ms>` at each locked end-stop while reporting live `/state`.
- **Direct Calibration & `VITAL` Writes**: Unlocks `turnout.feedback_regime`, `turnout.sim_transit_ms`, and (when `Cluster:POSITION` is composed) direct actuator positioning (`SET position.current=<pct | preset>`, `+=`/`-=`) and endpoint saves (`SET position.presets.NORMAL=<pct>`, `REVERSE=<pct>`).

---

## `Appliance:SENSOR`
*Track Circuit and Binary Contact Detector Appliance*

### ROLE
`sensor` — track occupancy detector (`832T`, `829T1`, `833T`), optical/IR detector, or discrete binary contact (such as a bungalow door monitor).

### TOPIC
`{channel}/sensor/{id}`

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:SENSOR`](../clusters/README.Clusters.md#clustersensor) *(required — read-only; `/state`: `ACTIVE` / `(ACTIVE)`)*
- [`Cluster:HOLDOFF`](../clusters/README.Clusters.md#clusterholdoff) *(required on optical/IR detectors; omitted on current-transformer detectors with hardware RC holdoff)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional)*

### BEHAVIOR & INVARIANTS
1. **Startup State**: Evaluates physical detection under active `holdoff.*` parameters and publishes `/state` (`ACTIVE` or `(ACTIVE)`).
2. **Read-Only Reporting**: Exposes no operational `/cmd` topic; publishes `ACTIVE` or `(ACTIVE)` on `/state` whenever filtered physical detection changes.

### MAINTENANCE MODE (`OFFLINE`)
- **`CYCLE <count> <dwell_ms>`**: No-op (read-only input appliance).
- **Direct Calibration & `VITAL` Writes**: Unlocks `holdoff.make_ms`, `holdoff.dropout_ms`, and `groups.members` while `/state` continues reporting live physical detection for shunt sensitivity and optical alignment testing.

---

## `Appliance:SIGNALMAST`
*Controlled, Absolute, and Permissive Signal Mast Appliance*

### ROLE
`signalmast` — 1-, 2-, or 3-head wayside signal mast (`830WAB`, `831WA`, `828WA`).

### TOPIC
`{channel}/signalmast/{id}`

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:MAST`](../clusters/README.Clusters.md#clustermast) *(required — `/cmd`: `<INDICATION> [LIT | (LIT)]`; `/state`: `<INDICATION>`)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional — e.g., station dimming or synchronized flasher group)*
- [`Cluster:APB`](../clusters/README.Clusters.md#clusterapb) *(optional — composed on autonomous `ABSOLUTE` / `PERMISSIVE` masts)*
- **Per-Head Sub-Components (`head.A`, `head.B`, `head.C`)**:
  - *Discrete color-light head*: `head.<X>.<COLOR>` (`RED`, `YELLOW`, `GREEN`, `LUNAR`) each composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) (+ optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))
  - *RGB / searchlight / pixel head*: `head.<X>` composing [`Cluster:COLOR`](../clusters/README.Clusters.md#clustercolor) (+ optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))
  - *Semaphore head*: `head.<X>` composing [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition) (+ optional `head.<X>.lamp` composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) / [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))

### BEHAVIOR & INVARIANTS
1. **Startup State**: Initializes all heads/blades to `STOP` and publishes `/state STOP` *(with `apb.stick = false` and autonomous evaluation starting from `STOP` and `(HOME) (DISTANT)` if `Cluster:APB` is composed)*.
2. **Transition & Lamp-Failure Reporting**: While a semaphore blade is moving between presets or a required head lamp is faulted, `/state` reports `STOP`:
   ```text
   /cmd (↦)        /state (⇤ 🅁)    Physical Meaning
   ---------       ------------    -------------------------------------------
   ----            STOP            Blade at Stop preset
   CLEAR           STOP            Actuator moving toward Clear preset
   ----            CLEAR           Actuator reaches Clear preset; reports CLEAR
   ```
3. **Approach Lighting (`LIT` / `(LIT)`)**: Optional `LIT` / `(LIT)` on `/cmd` toggles lamp illumination independently of `<INDICATION>` (`LIT` when omitted), allowing a controller to update a dark mast's indication without illuminating it.
4. **Autonomous APB Operation**: When [`Cluster:APB`](../clusters/README.Clusters.md#clusterapb) is composed, the mast exchanges `HOME` / `DISTANT` state on `/apb` with adjacent signals and tracks directional stick (`apb.stick`) locally.

### MAINTENANCE MODE (`OFFLINE`)
- **`CYCLE <count> <dwell_ms>`**: Steps the mast through each valid indication defined by `mast.rulebook` (or each head's lamps/presets) for `<count>` passes, holding each step for `<dwell_ms>`.
- **Direct Sub-Component & `VITAL` Writes**: Unlocks `mast.heads`, `mast.rulebook`, `apb.*`, and direct per-head overrides bypassing rulebook translation—e.g., `SET head.A.position.current=CLEAR` (or `+=`/`-=`), `SET head.A.position.presets.CLEAR=72.50`, `SET head.A.color.current=YELLOW`, `SET head.A.RED.onoff.on=true`.
- **Standalone APB Hold**: If `Cluster:APB` is composed, autonomous APB evaluation pauses and holds outbound `/apb` at `(HOME) (DISTANT)` while `OFFLINE`.

---

## `Appliance:TRAINORDER`
*Train Order Signal Face Appliance*

### ROLE
`trainorder` — one directional face (`E`/`W` or `N`/`S`) of a depot train-order board (`Gilroy:W`, `Gilroy:E`). A dual-face board is modeled as two independent 1-head appliances (one per traffic direction).

### TOPIC
`{channel}/trainorder/{id}`

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:MAST`](../clusters/README.Clusters.md#clustermast) *(required; `mast.heads = 1`, train-order `rulebook` mapped to `STOP` = Form 31, `APPROACH` = Form 19 / Slow, `CLEAR` = No orders)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional)*
- **Sub-Component (`head.A`)**:
  - *Semaphore arm (`UQ3` / `LQ2`)*: `head.A` composing [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition) + optional `head.A.lamp` ([`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) / [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))
  - *Color-light face*: `head.A.<COLOR>` ([`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff)) or `head.A` ([`Cluster:COLOR`](../clusters/README.Clusters.md#clustercolor) + optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))

### BEHAVIOR & INVARIANTS
1. **Startup State**: Initializes `head.A` to `STOP` and publishes `/state STOP`.

### MAINTENANCE MODE (`OFFLINE`)
- **`CYCLE <count> <dwell_ms>`**: Steps `head.A` through `STOP` → `APPROACH` → `CLEAR` for `<count>` passes, holding each step for `<dwell_ms>`.
- **Direct Sub-Component & `VITAL` Writes**: Unlocks `mast.rulebook` and direct `head.A` arm/lamp controls (`SET head.A.position.current+=1.00`, `SET head.A.position.presets.APPROACH=45.00`, `SET head.A.lamp.onoff.on=true`).

---

## `Appliance:CROSSING`
*Highway Grade Crossing Warning Appliance*

### ROLE
`crossing` — highway grade crossing flashers, bell, and gates driven by `/cmd ACTIVE` / `(ACTIVE)`.

### TOPIC
`{channel}/crossing/{id}`

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:CROSSING`](../clusters/README.Clusters.md#clustercrossing) *(required — `/cmd`: `ACTIVE` / `(ACTIVE)`; `/state`: `FLASHING GATES_DOWN` / `FLASHING (GATES_DOWN)` / `(FLASHING) (GATES_DOWN)`)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional)*
- **Sub-Components**:
  - `flasher` composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) (+ optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming))
  - `bell` *(optional)* composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff)
  - `gate.W`, `gate.E` *(optional)* each composing [`Cluster:POSITION`](../clusters/README.Clusters.md#clusterposition) (presets `UP`, `DOWN`)

### BEHAVIOR & INVARIANTS
1. **Startup State**: Initializes idle (flashers/bell off, gates `UP`) and publishes `/state (FLASHING) (GATES_DOWN)`.

### MAINTENANCE MODE (`OFFLINE`)
- **`CYCLE <count> <dwell_ms>`**: Runs `<count>` complete warning sequences (flashers and bell on, gates lower after `crossing.gate_delay_ms`, hold `<dwell_ms>`, gates raise, flashers and bell extinguish).
- **Direct Sub-Component & `VITAL` Writes**: Unlocks `crossing.gate_delay_ms` and independent sub-component bench controls (`SET flasher.onoff.on=true`, `SET bell.onoff.on=true`, `SET gate.W.position.current+=1.00`, `SET gate.W.position.presets.DOWN=85.00`) without triggering the full warning sequence (`crossing.flash_hz` and `crossing.bell_muted` remain `RW` anytime).

---

# Part II: Operator & Auxiliary Appliances (Human Control Surfaces & General Lighting)

Part II defines two general-purpose appliances for operator control surfaces, indicators, and layout lighting: **`CONTROL`** (multi-position levers, pushbuttons, and feedback indicators) and **`LAMP`** (binary, dimmable, or color-capable indicators and lighting).

## `Appliance:CONTROL`
*Smart Lever and Feedback Control Appliance*

### ROLE
`control` — multi-channel operator control surface combining switch/lever inputs (`SW1..N` via [`Cluster:LEVER`](../clusters/README.Clusters.md#clusterlever)), optional momentary pushbuttons (`BTN1..K` via [`Cluster:BUTTON`](../clusters/README.Clusters.md#clusterbutton)), and feedback indicator outputs (`LAMP1..M` via [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff)).

### TOPIC
`{channel}/control/{id}`

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:LEVER`](../clusters/README.Clusters.md#clusterlever) *(required — `/cmd`: complete ordered `LAMP1..M` frame; `/state`: complete ordered `SW1..N` frame)*
- [`Cluster:BUTTON`](../clusters/README.Clusters.md#clusterbutton) *(optional — `/event`: `BTN<k>` / `(BTN<k>)`)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional — e.g., `groups.members = ["ctc-panel"]` for synchronized panel dimming)*
- **Output Sub-Components** *(optional)*: feedback indicator outputs (`lamp.1..M`) each composing [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) (+ optional [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming), [`Cluster:COLOR`](../clusters/README.Clusters.md#clustercolor)).

### BEHAVIOR & INVARIANTS
1. **Startup State**: Configures `SW1..N`, `BTN1..K`, and `LAMP1..M` from `NV config`, extinguishes `lamp.1..M`, reads physical `SW1..N` contacts, and publishes `/state`.

### MAINTENANCE MODE (`OFFLINE`)
- **`CYCLE <count> <dwell_ms>`**: Walks `lamp.1..M` sequentially (or toggles all outputs together) for `<count>` passes at `<dwell_ms>` per step to verify panel indicators.
- **Direct Sub-Component & `VITAL` Writes**: Unlocks `lever.inputs`, `lever.outputs`, `lever.input_style`, `button.inputs`, `button.mode`, `groups.members`, and direct per-lamp testing (`SET lamp.1.onoff.on=true`). Physical `/state` (`SW1..N`) and `/event` (`BTN1..K`) remain live for switch and pushbutton continuity testing.

### EXAMPLE USAGE PROFILES

| Example Role | Example `{id}` | `/state` Inputs (`SW*`) | `/event` (`BTN*`) | `/cmd` Outputs (`LAMP*`) | Typical Channel Use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Local Switch / Derail Plate** | `Corporal:1`, `827` | `SW1 (SW2)`, `(SW1) SW2`, `(SW1) (SW2)` | *(or `MOMENTARY_1`/`2`)* | `LAMP1..2` | Normal/Reverse lever contacts and correspondence lamps |
| **Depot Train-Order Lever** | `Gilroy:W`, `Gilroy:E` | `SW1 (SW2)`, `(SW1) SW2`, `(SW1) (SW2)` | — | Optional `LAMP1..3` | 3-position order lever (Form 31 / Form 19 / Clear) and repeaters |
| **Electric Switch Lock / Dual-Control** | `lock:827`, `lock:835` | `SW1` / `(SW1)` | `BTN1` / `(BTN1)` (`LEVEL`) | `LAMP1` | Lock/hand status contact, unlock request button, release indicator |
| **Yard Route Selector** | `Watsonville:Arrival` | `SW1..SW<N>` (`ROTARY`) | — | Optional `LAMP1..N` | N-position rotary track selector and active-track lamps |
| **CTC Machine Panel Column** | `Corporal` | `SW1..SW<N>` | `BTN1` (`ONESHOT_RELEASE`) | `LAMP1..M` | Station switch/signal/lock levers, CODE button, indication jewels |

### WORKED CONFIGURATIONS

**(a) Electric Switch Lock (`{channel}/control/lock:827`)**
```text
# NV config
lever.inputs      = 1
lever.outputs     = 1
lever.input_style = "MAINTAINED"
button.inputs     = 1
button.mode       = "LEVEL"
groups.members    = ["corporal", "wayside-locks"]

# Sample wire traffic
⇤ /state  (SW1)     # Lock closed (normal/power)
⇤ /event  BTN1      # Crew presses and holds unlock request
↦ /cmd    LAMP1     # Controller illuminates release indicator
⇤ /event  (BTN1)    # Crew releases button
⇤ /state  SW1       # Crew opens lock / moves selector to HAND
```

**(b) CTC Machine Panel Column (`{channel}/control/Corporal`)**
```text
# NV config
lever.inputs      = 6
lever.outputs     = 9
lever.input_style = "MAINTAINED"
button.inputs     = 1
button.mode       = "ONESHOT_RELEASE"
groups.members    = ["ctc-panel"]

# Sample wire traffic
⇤ /state  SW1 (SW2) (SW3) SW4 SW5 (SW6)                  # Levers positioned
⇤ /event  BTN1                                           # CODE button released
↦ /cmd    LAMP1 (LAMP2) (LAMP3) LAMP4 LAMP5 (LAMP6) (LAMP7) (LAMP8) (LAMP9)
↦ {channel}/group/ctc-panel/maint  SET dimming.brightness=60
```

---

## `Appliance:LAMP`
*General Indicator, Maintainer Call, and Lighting Appliance*

### ROLE
`lamp` — binary, dimmable, or RGB/RGBA indicator, audible output, or lighting fixture (maintainer call lamp/horn, depot and structure lights, or layout room and backdrop lighting).

### TOPIC
`{channel}/lamp/{id}`

### COMPOSED CLUSTERS
- [`Cluster:MAINTENANCE`](../clusters/README.Clusters.md#clustermaintenance) *(required)*
- [`Cluster:ON/OFF`](../clusters/README.Clusters.md#clusteronoff) *(required — `/cmd`: `ON` / `(ON)`; `/state`: `ON` / `(ON)`)*
- [`Cluster:DIMMING`](../clusters/README.Clusters.md#clusterdimming) *(optional — PWM brightness and fade ramp)*
- [`Cluster:COLOR`](../clusters/README.Clusters.md#clustercolor) *(optional — calibrated palette, 16-bit RGB `($R, $G, $B)`, or HSB)*
- [`Cluster:GROUPS`](../clusters/README.Clusters.md#clustergroups) *(optional — synchronized room lighting, station, or panel dimming groups)*

### BEHAVIOR & INVARIANTS
1. **Startup State**: Initializes to `(ON)` (preserving saved `dimming.*` and `color.palette` settings) and publishes `/state (ON)`.

### MAINTENANCE MODE (`OFFLINE`)
- **`CYCLE <count> <dwell_ms>`**: Toggles `ON` / `(ON)` (or sweeps configured palette entries when `Cluster:COLOR` is composed) for `<count>` passes at `<dwell_ms>` per step.
- **Direct Calibration & `VITAL` Writes**: Unlocks `groups.members` and `IMPORT`; `onoff.on`, `dimming.*`, and `color.*` remain `RW` in both `ONLINE` and `OFFLINE`.
