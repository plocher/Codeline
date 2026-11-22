[![Codeline](images/codeline-logo.svg)](../README.md)
[Studio](Studio.md) · [Codeline](Codeline.md) · [Smart Appliances](SmartAppliances.md)  
↳ **▸ Roadmap**
---
---
# Roadmap: From Specification to a Running Railroad

The documents in this repository describe what Codeline is. This roadmap describes the order in which it gets built, what is taken from earlier work, and what is still undecided.

**Status:** draft for review. Milestones M0–M3 are planned in detail; later work is listed but not sequenced.

---

## 1. Ground Rules

1. **Correct first, in balance.** A first version must be correct; it does not have to be fast, polished, or general. Build what the current milestone needs and no more. This is not a license for carelessness: code that is hard to read is hard to prove correct. After a first version works, review it — "now, how would we have built this properly?" — and carry the answer into the next version.
2. **Test the real thing, through the simulator.** The design already includes the means to test: simulated drivers, an injected clock, and a transport loopback. Functional scenarios written against those are cheap and exercise real behavior. Until the first end-to-end version (M3) exists, do not write unit tests or build CI for their own sake.
3. **Process follows progress.** Until M3, commit to `main` with clear messages. Branches, pull requests, a changelog, and CI come after M3.
4. **All new code lands here.** Earlier repositories are *donors*: `FieldUnit` (Arduino library), `FieldUnit-Subdivision`, `FieldUnit-Studio`, and the KiCad `Railroad` project. They may be evolved in spikes to prove an idea.
5. **These docs are the requirements.** Where a donor disagrees with a document here, this repository wins. Where a donor's documentation is deeper or more correct (glossary, ADRs, primers), harvest it and bring it here.
6. **One thin slice before any breadth.** Prove every layer on one control point before scaling to seven appliances, seven interlockings, or a full authoring tool.
7. **No tooling hell.** JSON files, JSON Schema, plain Python generators, a plain host build. Nothing that needs its own project to maintain.

---

## 2. Decisions Already Made

| Topic | Decision |
| :--- | :--- |
| **Project file** | One JSON file is the Layout Data Model, the chosen reference data, and derived data together. It is Studio's native project file. |
| **Source vs. cache** | Within that file, `given` and `relations` are the source. `derived` is a reproducible cache, rebuilt and validated as editing happens. Vital logic consumes `derived`. |
| **Authoring today** | KiCad schematics, compiled by the Subdivision importer. The importer is one producer of the project file among several future ones (JMRI, CATS, C/MRI import); Codeline code never depends on KiCad. |
| **Layout model status** | Schema v3.4 from Subdivision is the starting point. It came from play-testing real designs and has not had an outside design review. It is open to change. |
| **Runtime configuration** | A runtime loads the project file (or a slice of it) at startup. An embedded logic editor on the field unit is not a goal. |
| **Schema format** | JSON source, checked by JSON Schema, with generators written in plain Python. Not YAML. |
| **Panel runtime** | A standalone host process runs the C++ engine. The on-screen panel talks to it over MQTT. |
| **First milestone** | A simulated CP Corporal, end to end. |

---

## 3. The Shape of the Problem

Earlier code is misaligned with this design at two seams, and nowhere else of consequence:

```text
  Office (panel, model board, test runner)
  ══ Code Line Interface ══════════   ← seam 1: no specification yet
  Vital Field Logic                   ← exists, tested, runs on a host
  ══ Smart Appliance Interface ════   ← seam 2: specified here, not built
  Smart Appliances (cores + drivers)
```

- **The middle exists.** FieldUnit's interlocking engine, control table, signal aspect policies, and scenario tests run on a host with time passed in as a parameter.
- **Seam 2 is specified but unbuilt.** FieldUnit's driver layer has two competing paths and an unresolved redesign. The Smart Appliance Interface replaces it.
- **Seam 1 is built but unspecified.** FieldUnit has working code-line transports and codecs, but three MQTT topic schemes exist across the donors, and the token order (which *is* the wire schema) is copied by hand in three places.

### The Code Line Is a Reflection of the Panel

On a US&S machine, each panel column was one code-line transaction, and the slots available (switch, signal, maintainer call, one extra) fixed its content. A digital code line removes the slot limit but keeps the connection:

- **Controls reflect levers. Indications reflect lamps** — on the panel and on the model board.
- A station's code chart is therefore *derived* from the office description in the project file, not written beside it.
- The content is a list of functions (an appliance and a word, such as switch `829` / normal). Tokens such as `829NWS` are how one encoding spells them. The 16-step US&S 506 limit belongs to one encoding, not to the contract.
- Indications are a superset of what the levers imply: the model board shows track occupancy and other lamps that have no lever.

---

## 4. Milestones

Each milestone ends in something that can be run and shown.

### M0 — Minimal Process

- A source layout for the first code, and one command that builds it and runs the functional scenarios on a host.
- Commit to `main` with clear messages.

**Shown by:** a fresh checkout builds and runs with that one command.

### M1 — Prove Each Seam Alone

Three pieces that do not depend on each other.

**M1a — TURNOUT vertical slice** *(seam 2)*
- `TickClock` / `ElapsedMillis`, `ITurnout`, `ITurnoutDriver`, `TurnoutCore`, `SimulatedTurnoutDriver`.
- `MqttTurnoutProxy` → in-memory mock broker → `MqttTurnoutAdapter`.
- One set of functional scenarios, run twice: direct, and through the MQTT loopback.
- **Schema seed:** `Cluster:MAINTENANCE` and `Cluster:TURNOUT` plus `Appliance:TURNOUT` written as JSON, with a Python generator that emits the C++ enums, the MQTT codec tables, and the test fixtures for this slice. Extend to the other clusters only after this works.

**Shown by:** the same scenarios passing on both paths, including faults injected from the simulated driver.

**M1b — Code Line Interface specification** *(seam 1)*
- A logical contract document in the same form as the cluster reference: control and indication functions, the atomic per-station transaction, positive control, what a rejected frame means.
- An MQTT binding document: one topic scheme, retention, token encoding.
- A chart deriver: given the project file, produce each station's ordered control and indication lists. Office, field, and test tools all read this output. Start from Subdivision's `tools/railroad/codeline.py`.

**Shown by:** the generated chart for CP Corporal matching the packets in the [Walkthrough](Walkthrough.md).

**M1c — Closed-loop spike on the engine**
- Codeline requires vital logic to trust only confirmed appliance state, and to substitute fail-safe values on `OFFLINE`, `FAULT`, or link loss. FieldUnit's engine drives outputs inside its own tick.
- Spike in the donor repo: put an asynchronous appliance boundary under the engine and run its existing scenario tests.

**Shown by:** a short written finding — adapt the engine, or rewrite the part below the control table.

### M2 — Simulated CP Corporal

- `SensorCore` and `MastCore` with simulated drivers, following the TURNOUT pattern. (`CONTROL` only if the electric lock at `827` is in the slice.)
- The vital engine, harvested per the M1c finding, bound to appliances through the Smart Appliance Interface.
- A host process that loads the project file, builds CP Corporal, and connects both interfaces to an MQTT broker.
- A scripted office that replays the Walkthrough move.

**Shown by:** a functional scenario that sends the Walkthrough's control packet and observes the same appliance traffic and indications, in the same order, including the cancel-with-time-locking case.

### M3 — On-Screen Panel and Model Board

- A panel for CP Corporal's columns and a model board for its track, generated from the project file. No hardcoded plant.
- It is an office on the Code Line Interface and nothing more: it holds no model of its own and computes no logic.
- A way to place and move a simulated train (shunt simulated sensors).

**Shown by:** a person lines the Walkthrough route with on-screen levers, presses CODE, and watches lamps and signals follow.

### After M3 *(not yet sequenced)*

- A review of the M0–M3 code: what to keep, and what to rebuild properly.
- Release hygiene: feature branches and pull requests, `docs/CHANGELOG.md`, CI (link check, host build, functional scenarios).
- The remaining appliances and clusters; the full schema.
- The rest of the subdivision; APB between control points.
- Python reference simulators and the MicroPython binding.
- Direct hardware drivers (`cpNode-IOX`, `PCA9685`) and the physical CTC machine.
- Proxy drivers (C/MRI, LocoNet, LCC).
- Studio authoring: editing `given` and `relations`, with `derived` rebuilt live.

---

## 5. Harvest Map

| Donor | Take | Leave |
| :--- | :--- | :--- |
| **FieldUnit** `src/` | `InterlockingPlant`, `ControlTable`, appliance logic, `SignalAspectPolicy`, scenario tests; `AarTextCodec` grammar; `ControlTransaction` / `IndicationVector`; `cTcMachine`'s panel-hardware split | Both driver paths; `MqttCodeLine` and its topics; declaration-order wire schemas; fixed `MAX_*` arrays; the `millis()` fallback |
| **FieldUnit** `docs/` | `GLOSSARY.md`, the AAR primer, accepted ADRs | — |
| **FieldUnit-Subdivision** | Schema v3.4 and reference tables; `spcoast.layout.json` as a fixture; `codeline.py`; the plant host's switch-motor model and self-test pattern | The KiCad importer chain (stays there as a tool); the older per-plant JSON formats; empty `runtime/*` placeholders |
| **FieldUnit-Studio** | Ideas: route synthesis (conflicts, aspect ceiling), the corridor matrix, console behavior rules, lever and jewel artwork | The code |
| **KiCad `Railroad`** | Symbol libraries as the record of what appliances and bindings exist in practice | — |

### What the First Studio Taught

- Geometry and topology were stored as one thing, and the canvas paid for it in special cases.
- The model was kept twice, in two languages, with logic on both sides.
- The console was polished before data drove it.
- Scope covered four workspaces before the vocabulary was stable.

M3 is shaped to avoid each of these.

---

## 6. Open Questions

1. **Stale `derived` data.** How does a consumer know the cache matches the source — a deriver version stamp, a content hash, or both? Runtimes should refuse a stale file.
2. **Where derivation lives.** It is Python in Subdivision today. Studio needs it on every edit. Port, embed, or call out?
3. **Layout model review.** An outside design review of schema v3.4, limited at first to what M2 and M3 read.
4. **Engine reuse.** Answered by M1c.
5. **Panel technology.** Simplest candidate: a static web page speaking MQTT over WebSockets to the broker. To be confirmed at M3.
6. **Meaning of `provenance`.** It pins a KiCad commit today. What does it record once Studio is the editor?
7. **Code line topic scheme.** Three exist in the donors; M1b picks one.
