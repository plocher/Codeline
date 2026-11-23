[![Codeline](../images/codeline-logo.svg)](../../README.md)
[Studio](../Studio.md) · **[Codeline](../Codeline.md)** · [Smart Appliances](../SmartAppliances.md)  
↳ [Walkthrough](../Walkthrough.md) · **▸ Code Line Interface** · [Code Line MQTT Binding](../bindings/CodeLineMQTT.md)
---
---
# Code Line Interface Reference

This document is the normative, **transport-agnostic** contract between the **Dispatcher Office** (CTC machine, on-screen panel, model board, test runner) and **Vital Field Logic** (see [**Codeline: Three Layers, Two Clean Interfaces**](../Codeline.md#2-three-layers-two-clean-interfaces)). It defines what a station's code line carries and what each end does with it.

It defines only the **logical contract**: functions, charts, transactions, and their rules. How a chart is carried on one transport, and how its functions are spelled on one encoding, live in companion binding specs:

- [**Code Line MQTT Binding**](../bindings/CodeLineMQTT.md) — one topic scheme, retention, the AAR `TOKEN` / `(TOKEN)` text encoding, and chart identity on the wire.

---

## 1. The Code Line Is a Reflection of the Panel

On a US&S machine each panel column was one code-line transaction, and the slots in the column (switch, signal, maintainer call, one extra) fixed what it carried. A digital code line removes the slot limit and keeps the connection:

- **Controls reflect levers.** Every position of every office lever is a control function.
- **Indications reflect lamps** — on the panel and on the model board. Indications are therefore a superset of what the levers imply: the model board shows track occupancy and other lamps that have no lever.
- **A station's chart is derived** from the office description in the project file. Nobody writes it by hand, and neither end builds its own: the office, the field, and every test tool read the same derived chart.
- **Content is functions, not spellings.** A function is an appliance and a word (switch `829` / `NORMAL`). `829NWS` is how the AAR encoding spells it; a US&S 506 encoding sends the same function as a step. Capacity limits such as the 506's seven function steps belong to an encoding, not to this contract.
- **The office holds no logic.** It never knows how many masts stand at a control point, how routes are wired, or whether a demand is safe. The field decides; the office learns the result only from indications.
- **One office per station.** Exactly one office is the source of a station's controls. Any number of listen-only repeaters (a second model board, a logger, a test tool) may read its indications; none of them sends controls. What stands behind the controls is the office's own business: its executive logic may pass panel levers straight through or add to them (an automated dispatcher, route stacking), but the field sees one source.

---

## 2. Vocabulary

| Term | Meaning |
| :--- | :--- |
| **Office** | Whatever sends controls and shows indications: a CTC machine, an on-screen panel, a model board, a scripted test. Non-vital. |
| **Field** | The Vital Field Logic serving one or more stations. It alone decides whether a control is acted on. |
| **Station** | The unit of addressing and atomicity on a code line. What a station is depends on the encoding (§7.1): on the AAR encoding it is an interlocking (`Corporal`); on a US&S 506 encoding it is one house. |
| **Function** | One named control or indication of a station: `(class, appliance, word)`, such as `(switch, 829, NORMAL)`. |
| **Class** | The kind of appliance a function belongs to: `switch`, `lock`, `signal`, `maintainer_call`, `track_circuit`, `lamp`. It fixes the words the function may use. |
| **Word** | What a function means for its appliance (`NORMAL`, `LEFT`, `OCCUPIED`, …). |
| **Control** | A function sent office → field. It states the dispatcher's intent; the field decides whether to act. |
| **Indication** | A function sent field → office. It reports the plant's proven state and is the office's only source of truth. |
| **Chart** | For one station: its name, its ordered list of control functions, and its ordered list of indication functions. |
| **Chart identity** | A short digest of a chart's content (§8). Both ends carry it so a mismatch is detected, never guessed around. |
| **Frame** | One transmission of one station's complete control or indication content. |
| **Asserted / dropped** | Each function in a frame is either asserted (true) or dropped (false). |
| **Positive-control group** | The functions of one lever or one appliance that are mutually exclusive (§5.2). |
| **Encoding** | How a frame is spelled (AAR tokens, US&S 506 steps). Belongs to a binding. |
| **Transport** | How a spelled frame is moved (MQTT, serial, in-process). Belongs to a binding. |

---

## 3. The Function Model

A function is **`(class, appliance, word)`**. The appliance is the bare name of the appliance inside its interlocking (`829`, `827T1`, `1MC`); for a signal it is the **signal lever's** number (`830`), never a mast — routes, masts, and aspects are the field's business.

### Function Words by Class

| Class | Source in the office | Controls | Indications | Group(s) |
| :--- | :--- | :--- | :--- | :--- |
| `switch` | Switch lever; selector choice | `NORMAL`, `REVERSE` | `NORMAL`, `REVERSE` | `{NORMAL, REVERSE}` both ways |
| `signal` | Signal lever | `LEFT`, `RIGHT`, `STOP` | `LEFT`, `RIGHT`, `TIME` | `{LEFT, RIGHT, STOP}` controls; `{LEFT, RIGHT, TIME}` indications |
| `lock` | Electric lock lever | `RELEASE` | `RELEASED`, `REQUEST` | — |
| `maintainer_call` | Maintainer call lever | `CALL` | `CALL` | — |
| `track_circuit` | Model-board or panel lamp | — | `OCCUPIED` | — |
| `lamp` | Model-board lamp repeating a field lamp | — | `LIT` | — |

The words are the ones ratified in FieldUnit-Subdivision ADR 0016 (`TIME`, `REQUEST`, `CALL` added by the owner), plus `LIT` for a repeated field lamp, which that ADR does not cover.

### Control Functions

| Function | Asserted means | Dropped means |
| :--- | :--- | :--- |
| `switch` / `NORMAL` | Demand the switch normal. | No demand for normal. |
| `switch` / `REVERSE` | Demand the switch reverse. | No demand for reverse. |
| `signal` / `LEFT` | Demand the signal cleared for movements toward the panel's left (the lever turned left). | No demand left. |
| `signal` / `RIGHT` | Demand the signal cleared toward the right. | No demand right. |
| `signal` / `STOP` | Demand the signal at stop (lever centered): cancel a cleared route, starting time locking if a train approaches. | No stop demand. |
| `lock` / `RELEASE` | Release the electric lock so a crew may operate the switch by hand. | Keep or return the lock locked. |
| `maintainer_call` / `CALL` | Light the field's maintainer call. | Extinguish it. |

### Indication Functions

| Function | Asserted means | Dropped means |
| :--- | :--- | :--- |
| `switch` / `NORMAL` | Switch proven normal and locked in correspondence. | Not proven normal. |
| `switch` / `REVERSE` | Switch proven reverse and locked in correspondence. | Not proven reverse. Both dropped: in transit or out of correspondence. |
| `signal` / `LEFT`, `RIGHT` | A healthy mast of any route **requiring** this lever and direction is confirmed displaying a proceed indication (its stop-proving relay is down)—whether this lever governs the route's mast directly or is co-required by a multi-section route. | No proceed confirmed for any route requiring this lever and direction. |
| `signal` / `TIME` | A route **governed by** this lever is running its time-locking countdown (`TER`), or is awaiting proof of `STOP` once neither `LEFT` nor `RIGHT` is asserted (§5.2 forbids `TIME` with `LEFT`/`RIGHT`). | No route governed by this lever is timing (or releasing with `GK` dropped). |
| `lock` / `RELEASED` | The lock is **not proven locked** — release granted, physically open, or appliance unproven/unhealthy (fail-safe indication so protecting signals stay at `STOP` and the office sees why). | Proven locked (contact closed and release withheld). *(At field restart or office loss, the field withholds release so the physical lock stays locked.)* |
| `lock` / `REQUEST` | A crew requests release at the field. Non-vital. | No request. |
| `maintainer_call` / `CALL` | The maintainer call is lit at the field. | Not lit. |
| `track_circuit` / `OCCUPIED` | The circuit is occupied — or unproven: the field reports occupied whenever it cannot prove vacancy. | Proven vacant. |
| `lamp` / `LIT` | The repeated field lamp is lit. | Dark. |

**Co-required levers on multi-section routes:** When a route is governed by one signal lever (`830` for mast `830WAB`) and also requires another signal lever (`828` LEFT) across an adjacent interlocking section with no intermediate mast, a confirmed proceed indication on `830WAB` asserts `LEFT` (`GK`) on **every** lever required by that route (`830WGK` and `828WGK`), and drops both as soon as `830WAB` ceases to display proceed. `TIME` (`TEK`) applies only to the route's governing lever (`830TEK`), never to a lever that the route only co-requires.

---

## 4. Station Content Is Complete and Ordered

- A chart lists **every** function of its station once, in a fixed order (§7.4).
- A frame carries **every** function of the chart, asserted or dropped, in chart order. There are no sparse or partial frames: "unchanged" is said explicitly by dropping every word of a group, never by leaving a function out.
- A frame with a missing, extra, repeated, unknown, or out-of-order function is **malformed**.

---

## 5. Transactions and Positive Control

### 5.1 The Control Transaction & Stored Demand Lifecycle

A **control transaction** is one control frame for one station, sent when the dispatcher presses CODE (or when a test sends one). It is **atomic per station**: the field applies all of it or none of it. One CODE press that covers several stations sends one independent transaction to each.

An accepted transaction updates the field's non-vital control demands (held in volatile application logic / stick circuits, always subordinate to vital locking):

- For a **positive-control group**, an asserted word becomes the new demand; a group with every word dropped leaves the previous demand **unchanged**.
- A **single function** (`lock` / `RELEASE`, `maintainer_call` / `CALL`) is a level: each frame states it, asserted or dropped.

Following prototype US&S / GRS / microprocessor CTC practice, stored demands obey three lifecycle rules:

1. **Refused or unfulfilled signal demands are dropped (no field-side route stacking).** A signal demand is held only long enough for switches commanded in the *same* atomic CODE frame to finish throwing and locking in correspondence—for example, sending `829NWS` + `830WGS` in one frame holds `830WGS` while `829` travels to Normal and clears `830WAB` once `829` locks, whereas sending `830WGS` alone while `829` is still moving from an earlier frame (or lined to no valid route) is dropped immediately. If vital locking refuses the demand (occupied block, conflicting route, time locking running), the signal demand is dropped—it does **not** clear automatically later when the hazard clears. The dispatcher must press CODE again. *(Optional route stacking/storage belongs in the office, which monitors indications and transmits a fresh CODE frame once the route is clear.)*
2. **Stick-drop on train passage (no automatic re-clear without Fleeting).** Once a signal clears and a train accepts it (dropping the approach or OS track relay), the signal drops to `STOP` and breaks its stored signal demand. A signal never re-clears automatically for a following train unless an explicit **Fleeting** (`FL`) control function (future extension) is asserted.
3. **Volatile fail-safe reset on field restart.** All non-vital control demands live strictly in volatile memory. After power loss or a field reboot, every stored demand clears to fail-safe: signals at `STOP`, switches left in their last locked physical position, electric locks locked, calls off. The field emits a full indication recall showing every signal at `STOP`, and vital logic enforces time locking (`TER`) if a train was approaching a previously cleared signal.

Because unfulfilled demands are not queued across transactions, whether a control frame is **unsafe** (§5.3) is evaluated directly on the incoming frame's asserted vital demands against live vital locking (and any switch alignment commanded in that same frame).

### 5.2 Positive-Control Rules

| Group state in one frame | Switch controls | Signal controls | Meaning |
| :--- | :--- | :--- | :--- |
| exactly one asserted | `NORMAL` *or* `REVERSE` | `LEFT` *or* `RIGHT` *or* `STOP` | That demand. |
| none asserted | `(NORMAL) (REVERSE)` | `(LEFT) (RIGHT) (STOP)` | Unchanged: keep the previous demand. |
| two or more asserted | `NORMAL REVERSE` | e.g. `LEFT STOP` | **Invalid.** The whole frame is malformed. |

Indication groups follow the same rule: `NORMAL` with `REVERSE`, or `LEFT` with `RIGHT`, is invalid; `TIME` with `LEFT` or `RIGHT` is invalid (a field never proves a proceed aspect while time locking runs on that lever). For indications, "none asserted" means "not proven" (switch out of correspondence, signal at stop), never "unchanged".

### 5.3 What the Field Does with a Frame

| The control frame is … | The field … |
| :--- | :--- |
| **Out of sync** — its chart identity is not the field's | Ignores it as a whole; counts it; sends its current indication frame (which carries the field's chart identity, so the office sees the mismatch). |
| **Malformed** — §4, or an invalid group (§5.2) | Ignores it as a whole, vital and non-vital controls alike; counts it; sends its current indication frame. |
| **Valid but unsafe** — any vital control would violate locking or conflict with another vital control in the frame | Ignores **every vital control** of the frame together (including any `STOP` demand in that same frame); still processes every non-vital control; sends no refusal. The office learns the outcome from indications: levers stay out of correspondence with their lamps. |
| **Valid and safe** | Applies it, then reports progress by indications as the plant moves. |

Vital controls: every `switch`, `signal`, and `lock` function. Non-vital: `maintainer_call` / `CALL`. The class names how the field processes a control; it does not make the code line vital.

A code-line control frame arrives as a simultaneous, unordered snapshot of levers—equivalent to inputs against a **mechanical locking bed**—so **no partial vital changes** are permitted when any vital control in the frame is unsafe (whereas internal vital logic continues running independently of the code line to drop signals on occupancy, faults, or link loss). Leaving a locked switch commanded to its *already-locked* position (e.g. `829NWS` while cancelling `830HS`) is a no-op and is not unsafe. Specifically, a frame is **valid but unsafe** if any of the following hold:

- A switch demand commands movement on a switch with any active lock (`DETECTOR`, `ROUTE`, `TIME`, or electric lock).
- Two signal demands in the same frame would clear conflicting routes.
- The frame simultaneously drops an electric lock release (`(WLS)`) and demands a signal over that lock (re-locking and clearing must be two separate transactions: restore the lock, then clear once proven locked).
- A `LEFT` / `RIGHT` signal demand targets a route with any occupied/unhealthy circuit along the route that would line (not only the OS), or targets a lever whose route is releasing or time-locking. *(A `LEFT` / `RIGHT` demand for which no route is lined is accepted and immediately dropped per §5.1 rule 1.)*
- An electric lock `RELEASE` (`WLS`) is demanded while any lock bit is set on that lock's switches, any route over those switches is not idle, or any mast on such a route is not proven at `STOP`. *(Checks are scoped to routes over that lock's switches, allowing simultaneous parallel moves elsewhere in the control point.)*

The field sends an indication frame after every control frame it receives (accepted or not), whenever any indication changes, and when it starts. It never acts on a control frame sent before its own start—ignoring both vital and non-vital controls (including `maintainer_call` / `CALL`): controls are intent at the moment of CODE, not state. Each binding states how it makes that checkable (for MQTT: [**Replay Protection**](../bindings/CodeLineMQTT.md#5-mqtt-transport-guarantees)).

### 5.4 What the Office Does with a Frame

| The indication frame is … | The office … |
| :--- | :--- |
| **Out of sync** or **malformed** | Discards it as a whole, keeps nothing from it, and shows the station as not current (lamps dark, a fault lamp or message) until a valid frame arrives. |
| **Valid** | Sets every lamp of the station from it. |
| **Absent** because the transport reports the field unavailable | Shows the station as not current. |

The office never infers a lamp from a lever, and never "corrects" a lamp it believes is wrong. Out-of-correspondence between a lever and its lamp is how the dispatcher learns that the field did not act.

---

## 6. Link Loss

- **Field loses the office**: stored demands stay as last accepted; the field keeps running its vital logic. No control is ever replayed. (Fallback to standalone operation is a modeler's option, see [**Codeline: Engineering Principles**](../Codeline.md#engineering-principles).)
- **Office loses the field**: the binding reports the station unavailable; the office shows it as not current.

---

## 7. Deriving the Chart from the Project File

The chart is computed from the project file by one deriver, [`tools/codeline_chart.py`](../../tools/codeline_chart.py). It reads the office description (`officeUnits`), the interlockings' appliances, and the code line definitions (`layout.codelines`); it writes nothing back.

### 7.1 Stations

Each interlocking names its code line (`interlockings.<ix>.relations.codeline`), and the code line's encoding says what a station is (`layout.codelines.<line>.derived.station`):

| Encoding | Station | Name |
| :--- | :--- | :--- |
| AAR tokens | The interlocking | `Corporal` |
| US&S 506 | One house of the interlocking | `Luchessa:Gilroy` |

### 7.2 Controls: from Levers and Selectors

- Each **desk lever** that serves an appliance (`derived.serves`) contributes the control functions of its positions, to the station of its interlocking (on a 506 line, the house its panel column resolves to).
- A **signal lever** contributes `LEFT`, `RIGHT`, and `STOP` for the signal named by the lever — not per mast.
- Each **selector** contributes `switch` / `NORMAL` and `REVERSE` for every switch named in any of its choices, to the station of that switch.

### 7.3 Indications: from Panel and Model-Board Lamps

- A lever's **own lamps** contribute the indication functions of its appliance (switch `NORMAL`/`REVERSE`, signal `LEFT`/`RIGHT`/`TIME`, lock `RELEASED`/`REQUEST`, maintainer `CALL`). A lever with no lamp for a word contributes no indication for it.
- A **model-board lamp** contributes, for each token it consumes, the function of the one appliance that reports that token: a track circuit, a switch's OS circuit (the circuit is the appliance), or a field lamp. The function goes to the **reporter's** station, wherever on the board the lamp sits. A lamp lit by an expression over several tokens contributes each token's function.
- A token that no appliance reports, or that several report, is a **problem**: the deriver lists it and exits non-zero. It never invents a function.

### 7.4 Ordering Rule

Functions are ordered by, in turn:

1. **Class**: `switch`, `lock`, `signal`, `maintainer_call`, `track_circuit`, `lamp`.
2. **Panel column** of the lever, ascending; functions with no column (selectors, model-board lamps) after those with one.
3. **Appliance name**, in natural order (`9` before `11`).
4. **Word**, in the class's order: `NORMAL, REVERSE`; `RELEASE, RELEASED, REQUEST`; `LEFT, RIGHT, STOP, TIME`.

5. **Plain name**, by Unicode code point, when two names have the same natural order (`01` before `1`).

A function reflected by several levers takes the **smallest** of their columns.

Lever functions therefore come first and model-board lamps last, and within the levers all switches precede all signals, as on the [**Walkthrough**](../Walkthrough.md) packets. The ordering rule and the positive-control groups of §5.2 are each stated once, in `tools/codeline_chart.py`. Encoders and decoders never sort: they read the chart's order. A runtime in another language reads the chart the deriver emits; it does not restate the rule.

### 7.5 Output

For each station the deriver emits its name, interlocking, house, code line, encoding, chart identity, and the ordered `controls` and `indications`. Each function entry carries `class`, `appliance`, `word`, the lever's `column` when it has one, the `sources` (project-file paths of the levers and lamps it reflects), and its AAR spelling `aar` as a convenience for the AAR binding. A signal's `LEFT` / `RIGHT` entries also carry `timetable`, the direction letter of the masts facing that side (needed to spell them, [**MQTT Binding §4**](../bindings/CodeLineMQTT.md#4-spelling-functions-as-aar-tokens)).

---

## 8. Chart Identity

A chart's **identity** is a digest of its content, not a hand-kept version number. It is the first **8 lowercase hexadecimal digits** of **SHA-256** over these bytes:

- UTF-8 text, one record per line, **every line ending in LF** (`0x0A`, including the last), fields separated by **TAB** (`0x09`), no other whitespace.
- Line 1: `CHART1` TAB *station*.
- Then one line per control function, in chart order: `C` TAB *class* TAB *appliance* TAB *word* TAB *token*.
- Then one line per indication function, in chart order: `I` TAB *class* TAB *appliance* TAB *word* TAB *token*.
- *token* is the function's spelling on the station's encoding (the AAR token on an AAR line); it is the empty string on an encoding that spells no tokens (US&S 506).

For example, a chart for station `Test` with one switch `1` begins `CHART1\tTest\nC\tswitch\t1\tNORMAL\t1NWS\n…`.

- **Spelling is part of identity.** Two charts with the same functions but different tokens (a mast renamed from `830NAB` to `830WAB` changes `830NGS` to `830WGS`) have different identities, so the receiver reports **out of sync**, the true cause, rather than **malformed**.
- Every frame carries the identity of the chart it was built from. A receiver whose chart identity differs treats the frame as **out of sync** (§5.3, §5.4). Office and field therefore detect a project file that changed on one side only, rather than misreading positions.
- `CHART1` versions this byte form. A change to it is a new tag.
- 32 bits detect accidental mismatch; they are not a security measure.

---

## 9. Out of Scope

- **Vital logic.** Locking, route selection, aspects, time-locking duration: the field's business. The code line carries demands and proven results only.
- **Masts, heads, and aspects.** No per-mast function exists. A mast's state for a richer model board is later work.
- **Addressing and capacity of a particular encoding**: 506 station addresses, step assignments, step limits.
- **Panel layout and lever wiring**: which pin is which lamp. The deriver reads the project file's result, not the drawing.
- **Telemetry, health, firmware info, and publishing the chart itself on the wire.** The chart comes from the project file on both ends; only its identity travels.
- **Security.** Authentication and authorization of offices.
- **Local control panels and fascia plates.** They act on appliances through the Smart Appliance Interface, not on the code line.
