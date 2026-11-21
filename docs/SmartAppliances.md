# Smart Appliances: An Open Specification for Model Railroad Hardware

Most model railroad control systems treat layout hardware as loose collections of raw actuator and sensor bits: `Output 14` drives a Tortoise motor, `Inputs 3 and 4` read microswitches, `Input 11` reads a block detector, and `Outputs 22–27` drive six LEDs on a signal mast. Even over MQTT—such as a single-topic JMRI turnout using `CLOSED`/`THROWN`—a raw motor bit is not a railroad switch.

**Smart Appliances** is an open, controller-agnostic MQTT specification modeled on **Zigbee / Matter capability clusters** and **AREMA functional interfaces**. It serves as the hardware layer beneath [**Codeline**](Codeline.md) and works with any layout control system. The normative specification lives in two companion references:

- [**Smart Appliance Capability Cluster Reference**](clusters/README.Clusters.md) — normative library of 14 general and railroad capability clusters (`↦ COMMANDS`, `⇤ EVENTS`, `ATTRIBUTES`, `RESTART BEHAVIOR`, and the `FAULT <TYPE>` registry).
- [**Smart Appliance Catalog**](appliances/README.Appliances.md) — concrete recipes for all 7 Smart Appliances (`Startup State`, operational timelines, sub-components, and `OFFLINE` maintenance mode).

---

## 1. Why a Smart Appliance Is More Than an "MQTT Motor"

Under **AREMA** signaling practice, vital logic interacts with wayside equipment through closed-loop **functional interfaces**—commanding an operation and requiring positive physical correspondence before the next step proceeds:

- **Raw turnout motor vs. Smart Switch (`TURNOUT`)**: A 1-topic MQTT turnout echoes its own command (`THROWN`) immediately, fooling control software into believing points are locked before the motor finishes moving. A Smart Switch accepts positive-control commands (`NORMAL (REVERSE)` or `(NORMAL) REVERSE`) and reports honest physical correspondence on `/state`: `NORMAL (REVERSE)` when locked Normal, `(NORMAL) REVERSE` when locked Reverse, and `(NORMAL) (REVERSE)` while points are in transit or out of correspondence (with `FAULT SHORTED_CONTACTS`, `FAULT STALL`, or `FAULT OVERCURRENT` on hardware failure). Crossovers (`815`/`815A`) and switch-plus-derail pairs (`827`/`827D`) remain independent endpoints coordinated by the controller above the interface.
- **Raw LEDs vs. Smart Signal Mast (`SIGNALMAST`)**: A Smart Signal Mast accepts a functional indication (`STOP`, `RESTRICTING`, `APPROACH`, `DIVERGING_CLEAR`, `CLEAR`) plus optional orthogonal approach lighting (`LIT` / `(LIT)`), translates it to physical heads under its provisioned `mast.rulebook` (`SPTCO-1960` flashing red vs. `SP-1990` lunar, `GCOR-1995`, etc.) and hardware binding, and reports back the indication physically displayed.

---

## 2. The Cluster & Two-Plane Architecture

Every Smart Appliance is an MQTT endpoint composed from reusable **Capability Clusters** speaking plain, self-describing `TOKEN` / `(TOKEN)` payloads across **four topics** split into two planes:

- **Operational Plane**: `↦ {appliance}/cmd` *(unretained)* and `⇤ {appliance}/state` *(`🅁` — the appliance's sole retained topic; always reports honest physical state)*.
- **Management Plane**: `↦ {appliance}/maint` *(unretained, plus channel broadcast `{channel}/maint` and optional `Cluster:GROUPS` `{channel}/group/{group}/maint`)* and `⇤ {appliance}/event` *(unretained `<NOUN> <payload>` for `OFFLINE`/`ONLINE`, `FAULT`, `ATTRIBUTES`, and `CONFIG`)*.

Core architectural rules:

- **Pure Hub-and-Spoke Endpoints**: Appliances store zero peer topic bindings in flash—the supervisory controller owns layout topology, locking, routing, and vital fail-safe substitution (on `/event OFFLINE`, `/event FAULT`, or LWT disconnect). Only [`Cluster:APB`](clusters/README.Clusters.md#clusterapb) opts into peer-to-peer `/apb` links for standalone computerless APB/MSS installations.
- **No Fake Capabilities & Strict Orthogonality**: A fixed-resistor GPIO LED exposes `ON/OFF` without `DIMMING` or `COLOR`; a Tortoise stall motor exposes `TURNOUT` without `POSITION`. Approach-lighting `(LIT)` → `LIT` preserves saved `DIMMING` and `COLOR`; turnout `/cmd` always speaks `NORMAL (REVERSE)` / `(NORMAL) REVERSE`, never `POSITION` percentages.
- **Unified Attribute Maintenance & Logical Restart**: Every attribute—root (`position.speed`) and sub-component (`head.A.position.current`, `head.A.color.palette.YELLOW`, `head.A.RED.onoff.on`)—is managed through [`Cluster:MAINTENANCE`](clusters/README.Clusters.md#clustermaintenance) (`SET`, `CLEAR`, `EXPORT`, `IMPORT`, `CYCLE`) under three access classes (`RO`, `RW`, `VITAL`). Exiting `OFFLINE` via `ONLINE` triggers a **logical restart** (the appliance's power-on initialization sequence, without dropping MQTT) so updated `NV config` takes effect from a deterministic startup state.
- **Implementation Details Stay Below the Seam**: Contact debouncing and wiring multiplexers (`3MUX2A`) reside inside hardware or firmware below the Smart Appliance interface.
- **1:1 Machine-Readable Schema**: Every cluster's commands, events, attributes, types, persistence, and defaults map directly to a single YAML/JSON schema driving validators, Studio UIs, firmware stubs, and generated documentation.

### 2.1 Capability Clusters → [Full Cluster Reference](clusters/README.Clusters.md)

| Family | Clusters | Role |
| :--- | :--- | :--- |
| **Universal & Addressing** | [`MAINTENANCE`](clusters/README.Clusters.md#clustermaintenance), [`GROUPS`](clusters/README.Clusters.md#clustergroups) | `OFFLINE`/`ONLINE` gate & logical restart, `IDENTIFY`, `EXPORT`/`IMPORT`, `SET`/`CLEAR`, `CYCLE`, `POST`, `FAULT <TYPE>` taxonomy, and decentralized multicast groups |
| **Lighting & Actuation** | [`ON/OFF`](clusters/README.Clusters.md#clusteronoff), [`DIMMING`](clusters/README.Clusters.md#clusterdimming), [`COLOR`](clusters/README.Clusters.md#clustercolor), [`POSITION`](clusters/README.Clusters.md#clusterposition) | Orthogonal binary `ON`/`(ON)`, gamma-corrected intensity/fade, AREMA & RGB/HSB color palette, and normalized `0.00%`–`100.00%` servo/stepper positioning |
| **Detection & Human Input** | [`HOLDOFF`](clusters/README.Clusters.md#clusterholdoff), [`LEVER`](clusters/README.Clusters.md#clusterlever), [`BUTTON`](clusters/README.Clusters.md#clusterbutton) | Local optical gap-bridging, hardware-neutral switch/lever frames (`SW1..N` / `LAMP1..M`), and momentary pushbuttons (`BTN1..K`) |
| **Railroad Domain** | [`TURNOUT`](clusters/README.Clusters.md#clusterturnout), [`SENSOR`](clusters/README.Clusters.md#clustersensor), [`MAST`](clusters/README.Clusters.md#clustermast), [`APB`](clusters/README.Clusters.md#clusterapb), [`CROSSING`](clusters/README.Clusters.md#clustercrossing) | Closed-loop switch correspondence, binary detection, rulebook signal display & approach lighting, optional MSS/APB tumble-down, and crossing protection |

### 2.2 Appliance Catalog → [Full Appliance Catalog](appliances/README.Appliances.md)

| Family | Appliance & Topic Base | Composed Clusters | Operational Summary |
| :--- | :--- | :--- | :--- |
| **Wayside Plant** | **[`TURNOUT`](appliances/README.Appliances.md#applianceturnout)** — `.../turnout/{id}` | `MAINTENANCE` (`post=false`), `TURNOUT`, opt. `POSITION`, `GROUPS` | `NORMAL (REVERSE)` / `(NORMAL) REVERSE`; `(NORMAL) (REVERSE)` in transit |
| **Wayside Plant** | **[`SENSOR`](appliances/README.Appliances.md#appliancesensor)** — `.../sensor/{id}` | `MAINTENANCE`, `SENSOR`, `HOLDOFF` *(optical/IR)*, opt. `GROUPS` | Read-only track/contact detector: `ACTIVE` / `(ACTIVE)` |
| **Wayside Plant** | **[`SIGNALMAST`](appliances/README.Appliances.md#appliancesignalmast)** — `.../signalmast/{id}` | `MAINTENANCE`, `MAST`, per-head clusters, opt. `APB`, `GROUPS` | `<INDICATION> [LIT\|(LIT)]` under `rulebook`; `STOP` while moving/faulted |
| **Wayside Plant** | **[`TRAINORDER`](appliances/README.Appliances.md#appliancetrainorder)** — `.../trainorder/{id}` | `MAINTENANCE`, 1-head `MAST`, `head.A`, opt. `GROUPS` | One directional face (`STOP`/`APPROACH`/`CLEAR`); dual board = 2 appliances |
| **Wayside Plant** | **[`CROSSING`](appliances/README.Appliances.md#appliancecrossing)** — `.../crossing/{id}` | `MAINTENANCE`, `CROSSING`, `flasher`, opt. `bell`, `gate.W`/`E`, `GROUPS` | `ACTIVE`/`(ACTIVE)` → `FLASHING` / `GATES_DOWN` |
| **Operator Surface** | **[`CONTROL`](appliances/README.Appliances.md#appliancecontrol)** — `.../control/{id}` | `MAINTENANCE`, `LEVER`, opt. `BUTTON`, `lamp.*`, `GROUPS` | Ordered `LAMP1..M` / `SW1..N` frames + `BTN1..K` for switch plates, locks, yard selectors, and CTC columns |
| **Auxiliary / Light** | **[`LAMP`](appliances/README.Appliances.md#appliancelamp)** — `.../lamp/{id}` | `MAINTENANCE`, `ON/OFF`, opt. `DIMMING`, `COLOR`, `GROUPS` | `ON`/`(ON)` — Maintainer Call, depot/structure lights, and RGB/dimmable room lighting |

---

## 3. Race-Free Messaging, Power-Up Safety, and Dynamic Discovery

Distributed layout hardware faces three classic failure modes—multi-topic race conditions, retained-message hazards on boot, and stale retained configuration:

- **Atomic Positive Control**: Complementary pairs always travel in one atomic payload (`NORMAL (REVERSE)` on `/cmd` and `/state`, `HOME (DISTANT)` on `/apb`), and `Cluster:LEVER` transmits complete ordered `LAMP1..M` / `SW1..N` frames. Proxies cannot reorder halves of a state pair or deliver partial frames.
- **Causal Closed Loop**: Separating `/cmd` (intent) from `/state` (physical confirmation) removes ordering ambiguity—an appliance updates `/state` only after physical completion, and vital logic never advances until `/state` confirms it.
- **No Retained Commands**: Because `/cmd` is never retained and `maintenance.post` defaults to `false` on `TURNOUT`, waking nodes never throw switches unexpectedly on power-up before occupancy detectors finish booting.
- **No Retained Configuration (Dynamic Discovery)**: Configuration is never retained on the broker, eliminating zombie entries after boards are unplugged or renamed. Controllers discover live inventory by broadcasting `EXPORT CONFIG` on `{channel}/maint` (jittered `0–250 ms` replies on `/event CONFIG`), supplemented by boot-time `/event CONFIG` hot-plug announcements.

See [**Walkthrough**](Walkthrough.md) for concrete wire traces of routing, offline servo calibration, and dynamic discovery.

---

## 4. Three Ways to Implement Any Smart Appliance

![Implementing Smart Appliances](images/04-smart-vs-wired-realization.svg)

Above the Smart Appliance Interface, every appliance speaks the exact same API. Below it, any endpoint can be fulfilled by one of **three interchangeable implementations**:

### 1. Simulated Appliances (Built-In Virtual Railroad)
Software implementations fulfill the full operational and maintenance APIs in memory—rendering signal aspects on screen, stepping turnouts through `(NORMAL) (REVERSE)` transit timers, and injecting virtual train shunts on track sensors before any hardware is wired.

### 2. Direct Smart Appliances (Microcontroller Cluster Composition + Hardware Binding)
An ESP32, Raspberry Pi Pico W, or bungalow controller driving local `cpNode-IOX` (`MCP23017`), `PCA9685` servo/PWM boards, NeoPixels, Tortoise motors, and microswitches runs the appliance directly:

- **Signal Masts across Hardware Types**: Provision one `mast.rulebook` (`SP-1990`, `SPTCO-1960`, `GCOR-1995`) and bind heads to RGB H-2 searchlights (`Cluster:COLOR`), 3- or 4-lamp discrete heads (`Cluster:ON/OFF`), 2-wire `3MUX2A` multiplexed decoders, NeoPixel chains, or servo semaphores (`Cluster:POSITION`). The operational `/cmd` remains identical across all of them.
- **Flexible Switch Feedback Regimes**: Under [`Cluster:TURNOUT`](clusters/README.Clusters.md#clusterturnout), `turnout.feedback_regime` supports `2-CONTACT` (Normal + Reverse microswitches), `1-CONTACT` (Normal switch + emulated Reverse), or `SIMULATED` (timed transit emulation when no point contacts are installed).

See [**Studio: Hardware Bindings and Benchwork Wiring Documentation**](Studio.md#4-hardware-bindings-and-legacy-layout-import) for pin bundles, active-low defaults, cardstock board templates, and cable loom drawings.

### 3. Proxy Appliances (Wrapping External `C/MRI`, `LocoNet`, `LCC`, and JMRI Buses)
Software **Proxy Appliances** present a standard Smart Appliance API upward while bridging low-level bits, packets, or events across existing `C/MRI`, `LocoNet`, `LCC`, or legacy MQTT buses downward:

- **Upgrading 1-Bit Turnouts**: Wrapping a 1-bit C/MRI or MQTT turnout output with `turnout.feedback_regime = SIMULATED` gives it realistic `(NORMAL) (REVERSE)` transit correspondence.
- **Bit-Mapped vs. Smart Bus Nodes**: Against a classic C/MRI SMINI, the Proxy Appliance runs hardware binding and 1 Hz flashing in `OB[]` on the host; against smart Wi-Fi C/MRI nodes (such as Andrew Deak's nodes), the Proxy flips target bits in `OB[]` while the remote node executes incandescent fade ramps and servo profiles locally.
