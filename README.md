[![Codeline](docs/images/codeline-logo.svg)](README.md)
[Studio](docs/Studio.md) · [Codeline](docs/Codeline.md) · [Smart Appliances](docs/SmartAppliances.md)
---
---
# Codeline

**Codeline** is a system for designing, simulating, and operating period-correct model railroads—bridging the gap between a prototype trackplan and live trackside hardware without the complexity and time-sink of doing it yourself.

Model a subdivision using real railroad operating regimes (CTC, Rule 251, ABS/APB, Yard Limits, DTC/TWC), operate it immediately in simulation, and connect physical layout hardware incrementally across a clean, hardware-agnostic MQTT boundary.

![From Layout Vision to Operating Railroad](docs/images/00-codeline-overview.svg)

---

## Three Actors, Two Clean Interfaces

Codeline splits layout design, vital railroad logic, and physical hardware into three distinct actors:

1. **[Studio](docs/Studio.md)** — *The workshop that helps you create, simulate, and wire your layout.*
   Sketch the trackplan, assign era-specific operating regimes and appliances, test meets in real time with animated AREMA relay diagrams, discover live hardware dynamically, import existing JMRI/CATS/C/MRI setups, and generate under-benchwork wiring templates, cable looms, and ready-to-flash microcontroller firmware.
2. **[Codeline](docs/Codeline.md)** — *The prototypical logic that operates your layout.*
   Connects dispatcher offices and local panels across the **Code Line Interface** to hardware-agnostic **Vital Field Logic** (detector, route, approach, time, and switch locking plus declarative route/signal resolution). Run the vital logic on an office computer, a RasPi in a cTc Machine, distributed in ESP32s under the layout, or in a hybrid mix.
3. **[Smart Appliances](docs/SmartAppliances.md)** — *The smart devices that bring your layout to life.*
   An open, transport-agnostic hardware specification modeled on Zigbee/Matter capability clusters and AREMA functional interfaces, with mechanical in-process (**[C++](docs/bindings/Cpp.md)**, open to MicroPython) and network (**[MQTT](docs/bindings/MQTT.md)**) bindings. Every switch, sensor, signal mast, train-order board, grade crossing, control panel, or lamp implements the same typed async API — whether simulated in software, running natively on a microcontroller, or proxied over existing `C/MRI`, `LocoNet`, or `LCC` hardware.

![Codeline Architecture — Hardware-Agnostic Logic Over Smart Appliances](docs/images/02-architecture-layers.svg)

---

## Documentation Guide

| Document | Focus |
| :--- | :--- |
| **[Studio](docs/Studio.md)** | Trackplan authoring, live simulation, dynamic hardware discovery, and code/documentation generation workflow |
| **[Codeline](docs/Codeline.md)** | Operating regimes, positive control & fail-safe design, Code Line Interface, declarative signal solver, deployment topologies, and standalone fallback |
| **[Smart Appliances](docs/SmartAppliances.md)** | Typed async logical hardware model, cluster & catalog overview, and mechanical C++ / MQTT bindings |
| **[Building Smart Appliances](docs/BuildingAppliances.md)** | Physical and driver layer: shared Reference Appliance Core, Simulated / Direct MCU / Proxy drivers, pin bundles, Active-Low wiring conventions, and benchwork shop documentation |
| **[Walkthrough](docs/Walkthrough.md)** | End-to-end CP Corporal westbound move (dispatcher levers → vital logic → MQTT wire trace → APB tumble-down), offline servo calibration, and dynamic discovery |
| **[SPCoast Example](docs/Background.md)** | Reference 1992–1996 Southern Pacific Coast Line setting (Gilroy–Watsonville), timetable conventions, rulebook & equipment eras, and US&S / AREMA nomenclature guide |
| **[Capability Clusters](docs/clusters/README.Clusters.md)** | Transport-agnostic normative reference for all 14 general and railroad capability clusters (`MAINTENANCE`, `GROUPS`, `POSITION`, `TURNOUT`, `MAST`, `APB`, etc.) |
| **[Appliance Catalog](docs/appliances/README.Appliances.md)** | Transport-agnostic normative catalog of all 7 Smart Appliances (`TURNOUT`, `SENSOR`, `SIGNALMAST`, `TRAINORDER`, `CROSSING`, `CONTROL`, `LAMP`) |
| **[C++ Binding](docs/bindings/Cpp.md)** | Embedded C++ in-process binding, Reference Appliance Core + Driver Port, local/mirror/remote MQTT glue stack, and deterministic functional test harness |
| **[MQTT Binding](docs/bindings/MQTT.md)** | Mechanical MQTT transport and `TOKEN`/`(TOKEN)` wire encoding binding, retention, LWT, jittered dynamic discovery, and `CONTROL` wire traces |
| **[Roadmap](docs/Roadmap.md)** | Implementation order, milestones up to an on-screen CP Corporal panel, what is harvested from earlier work, and open questions |

**All of the above as a single book:** [Codeline-Complete.pdf](docs/pdfs/Codeline-Complete.pdf) — every document as one multi-chapter PDF with a table of contents and bookmarks. Rebuild it with `python3 tools/build_docs_pdf.py` (setup instructions are at the top of that script).
