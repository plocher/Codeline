[![Codeline](images/codeline-logo.svg)](../README.md)
**▸ Studio** · [Codeline](Codeline.md) · [Smart Appliances](SmartAppliances.md)  
↳ [SPCoast Example](Background.md)
---
---
# Studio: Designing, Simulating, and Wiring Your Layout

**NOTE: This document describes an aspirational future tool** that today is represented by a KiCad environment with railroad symbols and schematic pages.

**Studio** is the interactive design, simulation, and commissioning workshop for [**Codeline**](Codeline.md) and [**Smart Appliances**](SmartAppliances.md). Instead of manually programming table rows and panel conditionals, you describe the railroad you want to operate, test it live in simulation, and generate the hardware bindings, firmware, and shop wiring documentation directly from one model.

---

## 1. Bridging the Gap Between Trackplan and Wiring

Traditional model railroad control software asks you to build the railroad twice—once as physical benchwork and once as low-level software plumbing:

- In **JMRI PanelPro**, you connect an I/O bus (`C/MRI`, `LocoNet`, or `LCC`), populate rows of Turnout, Sensor, Signal Head, and Signal Mast tables, draw panel icons one by one, and write Logix conditionals for every block boundary, switch correspondence sensor, route lock, and panel lamp.
- **CATS** (*Computer / Crandic Automated Traffic System*) raises the abstraction to magnet-board diagrams that evolve into control panels with built-in interlocking and APB logic, though it remains layered on JMRI and requires a central computer online.
- Custom BASIC, C, or Python control scripts following Bruce Chubb's **C/MRI handbooks** give full control but require a commitment to BASIC programming all the details as the layout evolves.

**Studio** starts from prototype railroad intent. You lay out the high level design, choose an era and rulebook, drop functional appliances onto a trackplan, and let Studio synthesize the vital logic, control surfaces, and physical wiring artifacts automatically.

---

## 2. Authoring and Live Simulation

Inside Studio, layout design behaves like an interactive model board that understands railroad engineering:

1. **Sketch the Subdivision & Assign Operating Regimes**: Draw your corridor (check out a [1990s Southern Pacific Coast Line design](Background.md)), choose your own Railroad, and Era, and assign operating regimes—CTC for dense junctions, Rule 251 on directional double track, APB on single-track runs, Yard Limits and DTC/TWC on branches.
2. **Place Smart Appliances with Prototype Defaults**: Drop switches and derails (`TURNOUT`), track circuits (`SENSOR`), signal masts (`SIGNALMAST`), train-order boards (`TRAINORDER`), grade crossings (`CROSSING`), control surfaces (`CONTROL`), and auxiliary outputs (`LAMP`) onto the plan. Studio infers sensible defaults from context:
   - A mainline crossover between `MT1` and `MT2` defaults to a power-operated switch pair with point feedback, `OS` track circuits, and dispatcher levers.
   - A mainline siding switch defaults to an electrically locked switch with local fascia controls; an industrial lead switch defaults to a locally controlled motorized switch with no track circuit or dispatcher lever.
   - A 2-head controlled mast under `SP` / `1995` automatically receives the Southern Pacific route-signaling indication table and US&S H-2 searchlight head defaults—without hand-writing aspect equations.
3. **Simulate Before Wiring a Single Board**: Studio embeds [Codeline's **Vital Field Logic**](Codeline.md) and the simulated [Appliance Core](bindings/Cpp.md#3-reference-appliance-core--pluggable-driver-port) inside the editor. Routes, control tables, and signaling `HOME`/`DISTANT` links synthesize on the fly. Drag a virtual train onto the track, line routes on a virtual US&S CTC console, inspect **Code Line** and **Smart Appliance** traffic, and watch a live **animated AREMA relay diagram** as lock, stick, and timer relays pick up and drop.
4. **Test Eras & Operational Scenarios**: Switch eras from 1960s flashing-red restricting aspects to 1990s lunar-white aspects and watch signal heads update across the subdivision, or test a meet between CP Corporal and CP Sargent before cutting a single piece of plywood.

---

## 3. Dynamic Hardware Discovery & Commissioning

As physical electronics come online under the benchwork, Studio discovers and commissions appliances over the [**Smart Appliance MQTT Binding**](bindings/MQTT.md) (including co-located bungalow nodes exposing an optional [**MQTT Mirror**](bindings/Cpp.md#4-composable-glue-stack-local-mirror--remote-mqtt)) without relying on stale retained broker state (see [**Walkthrough: Dynamic Studio Discovery**](Walkthrough.md#3-dynamic-studio-discovery) and [**Offline Servo Calibration**](Walkthrough.md#2-offline-servo-calibration)):

- **Zero-Zombie Inventory**: Studio subscribes to `{channel}/+/+/event` and broadcasts `EXPORT CONFIG` on `{channel}/all`. Every live appliance replies after `0–250 ms` random jitter with its unretained `/event CONFIG <data>`, letting Studio build an exact live inventory of composed clusters, sub-components, non-volatile settings, and `Cluster:GROUPS` memberships.
- **Hot-Plug Arrival**: Newly powered boards announce themselves automatically via boot-time `/event CONFIG`.
- **Bench Calibration & Identification**: Studio sends `IDENTIFY` pulses to locate boards under the benchwork, places individual appliances `OFFLINE` to unlock `VITAL` attributes (`position.current` nudging, `position.presets`, `holdoff.dropout_ms`, `mast.rulebook`, `groups.members`), runs `CYCLE` break-in sweeps, and returns appliances `ONLINE` via a clean logical restart.
- **Schema-Driven Tooling**: Because the [**Capability Cluster Reference**](clusters/README.Clusters.md) and [**Appliance Catalog**](appliances/README.Appliances.md) form a transport-agnostic 1:1 data model, a single machine-readable YAML/JSON schema can drive Studio's provisioning forms, runtime validators, language binding headers/stubs (e.g., C++ or MicroPython), MQTT codecs, and reference docs.

---

## 4. Hardware Bindings and Legacy Layout Import

Studio connects logical trackplan appliances to physical hardware incrementally—allowing simulated endpoints, native microcontrollers, and legacy buses to run side by side on the same layout:
- **Smart Appliances** expose a functional interface over MQTT that is discoverable and decoupled.

- **A smart appliance builder** helps you turn your layout devices into smart appliances for use directly in Studio.

- **Default Pin Bundles *or* Manual "Rat's-Nest" Routing**: For new construction, each hardware binding suggests a contiguous **pin bundle** (e.g., 6 consecutive outputs on `IOX0` for a 2-head mast, or `M, N, R, OCC` for a switch) so an appliance drops onto a board port in one step. For existing layouts whose wiring grew organically across multiple cards, manual per-wire routing documents the exact pins as wired.
- **Layout-Proof Active-Low Defaults**: To avoid phantom occupancy from floating inputs and reduce short-circuit risk on long harness runs, **Active-Low** is the universal hardware-binding default (grounding inputs to assert, sinking current on outputs), with cascading overrides from layout-wide down to chip type, board, bank, or individual pin.
- **Import Existing JMRI, CATS, or C/MRI Configurations**: Rather than re-entering hundreds of hardware addresses—or keeping JMRI running solely as a middleman—Studio imports existing JMRI, CATS, or C/MRI configuration files to instantiate [**Proxy Appliances**](SmartAppliances.md#3-proxy-appliances-wrapping-external-cmri-loconet-lcc-and-jmri-buses) and drive `C/MRI`, `LocoNet`, and `LCC` buses directly.

---

## 5. Benchwork-First Wiring Documentation & Code Generation

From one verified layout model, Studio generates ready-to-deploy microcontroller firmware/scripts (such as Arduino/ESP32 C++ or future MicroPython targets—combining Reference Appliance Cores, hardware drivers, and selected Local/Mirror/MQTT binding glue), C/MRI configurations, and physical shop documentation built for working under the layout with a flashlight:

![Generated Benchwork Documentation: Under-Board Cardstock Template and Wire Harness Diagram](images/05-wiring-documentation.svg)

- **1:1 Under-Board Cardstock Mounting Templates**: For I/O boards such as `cpNode-IOX` (and its PWM and Servo variants), Studio prints a 1:1 scale cardstock backing sheet that mounts directly behind the PCB on its standoffs. Around the perimeter—aligned with each 4-pin pluggable Phoenix terminal block (`Pins 16–1`), daughterboard socket (`IOX-Darlington`, `IOX-Monitor`), I²C header, and address DIP switch (`0x20`)—it prints wire names, colors, appliance IDs, and `[EMULATED]` / `[SPARE]` markers so the board carries its own permanent under-layout documentation.
- **Workbench Wire Harness & Cable Loom Diagrams**: Aviation-style harness drawings (`Loom L-830WAB`, `Loom L-829`) show the exact fan-in from each appliance pigtail, through the multi-conductor cable trunk, to the numbered pins on the mating Phoenix plugs (`J2`, `J12`, `J11`, `J16`) so harnesses can be built, labeled, and continuity-tested at the workbench.
- **NMRA Electrical & Prototype Bungalow Inspection Packets**: Studio outputs block, turnout, and signal schematics suitable for **NMRA Master Model Railroader (Electrical AP)** documentation, alongside **AREMA / DOT-style bungalow checkout sheets** for verifying lamp current sink, polarity, and OS drop tests.
