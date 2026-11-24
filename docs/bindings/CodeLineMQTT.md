[![Codeline](../images/codeline-logo.svg)](../../README.md)
[Studio](../Studio.md) · **[Codeline](../Codeline.md)** · [Smart Appliances](../SmartAppliances.md)  
↳ [Walkthrough](../Walkthrough.md) · [Code Line Interface](../codeline/README.CodeLine.md) · **▸ Code Line MQTT Binding**
---
---
# Code Line MQTT Transport &<BR>&nbsp;AAR Token Encoding Binding

This document defines the normative, **mechanical projection** of the transport-agnostic [**Code Line Interface**](../codeline/README.CodeLine.md) onto **MQTT**, using AAR `TOKEN` / `(TOKEN)` text frames.

It follows the same **Logical Contract → Transport Binding → Wire Encoding** pattern, and the same conventions, as the [**Smart Appliance MQTT Binding**](MQTT.md): a `{channel}` prefix, plain-English text payloads readable with `mosquitto_sub`, no retained commands, and retained state with broker Last Will and Testament (LWT).

---

## 1. Topic Scheme

Every station is addressed by the code line **channel** and its **station** name:

```text
{station} = {channel}/{name}
```

### Legend
- **{channel}**: A unique per-layout MQTT topic prefix for the code line (`/layout/SPCoast/codeline`). It is a separate channel from the appliances' (`/layout/SPCoast/appliances`).
- **{name}**: The station's name from its chart (`Corporal`), with each space replaced by `-` (`Luchessa:Gilroy` stays as written; `Christopher North` becomes `Christopher-North`).
- **`↦`**: The field **subscribes** (Office → Field). **`⇤`**: The field **publishes** (Field → Office). **`🅁`**: **Retained** by the broker, with LWT.

| Logical Surface | MQTT Topic | Dir | Retained | QoS | Semantics |
| :--- | :--- | :---: | :---: | :---: | :--- |
| Control transaction | `{station}/controls` | `↦` | No | **0** | One complete control frame per CODE press. Never retained and never redelivered (§5), so a field never acts on an old CODE press. |
| Indication frame | `{station}/indications` | `⇤` | **`🅁`** | 1 | The station's latest complete indication frame. Retained, so an office that starts late lights every lamp at once. The field's LWT publishes `UNAVAILABLE` here, retained. |

These two topics are the whole binding. Offices subscribe to `{channel}/+/indications`; a field subscribes to `{station}/controls` for each station it serves.

### Why This Scheme

Three schemes existed in the donors: `/layout/<L>/codeline/<cp>/{controls,indications,json,telemetry,info}` (FieldUnit ADR 0001), `ctc/SPCoast/SouthCTC/codeline/` (earlier Subdivision snapshots), and `codelines/.../controls` (earlier Walkthrough drafts). This binding keeps FieldUnit's root and leaf words and drops the rest:

- **`{channel}/{name}/…` has the same shape as `{channel}/{role}/{id}/…`** on the appliance side, under the same `/layout/<L>/` root, so one broker ACL and one naming habit cover both interfaces.
- **A separate code line channel** keeps a station name from ever colliding with an appliance role, and lets an office subscribe to the code line without seeing appliance traffic.
- **`controls` and `indications`** are the words of the contract and of all three donors.
- **No `json`, `telemetry`, or `info` topics.** The chart is derived from the project file on both ends, and only its identity travels (§3). Telemetry is out of scope. This also keeps the binding free of retained configuration, as on the appliance side.
- **No processor or machine name in the path** (`SouthCTC` in the older donor topic): a station is the same station whichever office talks to it.

The project file's `layout.codelines."AAR Tokens".given.Topic` now carries `/layout/SPCoast/codeline` directly.

---

## 2. Frame Grammar

```text
control-frame    = "CHART" SP chart-id 1*(SP term)
indication-frame = "CHART" SP chart-id 1*(SP term)  /  "UNAVAILABLE"
chart-id         = 8HEXDIG-lowercase
term             = token / "(" token ")"
token            = 1*(ALPHA / DIGIT)          ; uppercase as spelled in §4
```

- **`TOKEN`** asserts a function; **`(TOKEN)`** drops it.
- Terms are separated by single spaces, and appear **exactly** in chart order, one per chart function. Readers accept any run of ASCII whitespace—space (`0x20`), tab (`0x09`), CR (`0x0D`), or LF (`0x0A`)—between terms (no other whitespace characters); they compare tokens exactly, case included.
- A frame whose `CHART` identity differs from the reader's chart is **out of sync**. A frame with any missing, extra, repeated, unknown, or out-of-order term, or any invalid positive-control group, is **malformed**. Either way the whole frame is rejected, with the consequences in [**Code Line Interface §5.3–5.4**](../codeline/README.CodeLine.md#53-what-the-field-does-with-a-frame).
- `UNAVAILABLE` appears only as the field's LWT (or as its own clean shutdown message). An office reading it shows the station as not current; the next indication frame replaces it.

---

## 3. Chart Identity on the Wire

Every frame opens with `CHART <id>`, the 8-digit chart identity of [**Code Line Interface §8**](../codeline/README.CodeLine.md#8-chart-identity). The identity includes the AAR token of every function, so a chart whose tokens change (a mast renamed from `830NAB` to `830WAB`) gets a new identity, and the receiver reports *out of sync*, not *malformed*. It travels in the frame, not on a topic of its own, so:

- a retained indication frame left by an older field build is recognized as out of sync by a newer office, and the reverse;
- the identity and the content it governs can never be split by the broker.

When the field rejects an out-of-sync control frame, it republishes its own indication frame. The office then sees the field's identity, can report "field chart `6e560295`, office chart `85973c19`", and shows the station as not current. Neither side tries to translate between charts.

---

## 4. Spelling Functions as AAR Tokens

A control token ends in `S`; an indication token ends in `K`. `{n}` is the function's appliance.

| Class | Word | Control token | Indication token |
| :--- | :--- | :--- | :--- |
| `switch` | `NORMAL` / `REVERSE` | `{n}NWS` / `{n}RWS` | `{n}NWK` / `{n}RWK` |
| `signal` | `LEFT` / `RIGHT` | `{n}{d}GS` | `{n}{d}GK` |
| `signal` | `STOP` | `{n}HS` | — |
| `signal` | `TIME` | — | `{n}TEK` |
| `lock` | `RELEASE` | `{n}WLS` | — |
| `lock` | `RELEASED` / `REQUEST` | — | `{n}WLK` / `{n}WLQK` |
| `maintainer_call` | `CALL` | `{n}S` (`1MCS`) | `{n}K` (`1MCK`) |
| `track_circuit` | `OCCUPIED` | — | `{n}K` (`829T1K`), or `{n}` if the name already ends in `K` |
| `lamp` | `LIT` | — | `{n}K` (`WFLK`) |

**`{d}` — the signal's timetable letter.** A signal function is spelled with the timetable direction of that side of the lever, as in [**Background §4**](../Background.md#4-uss-code-line-and-arema-vital-relay-guide): `W` for westward (`830WGS`), `E` for eastward (`830EGS`). The letter is read from the project file, not configured: it is the direction letter in the names of the lever's masts that face that side (mast id = signal number, direction, heads; [**Background §2**](../Background.md#2-timetable-orientation-and-plant-naming-conventions)). Masts `830WAB` facing left give `LEFT` → `W`. If the masts on one side disagree, or none face it, the deriver reports a problem and spells nothing.

### Example: CP Corporal

As derived from the project file (`python3 tools/codeline_chart.py --station Corporal`, using `tools/spcoast.layout.json`), with switch `829` lined normal, electric lock `827` locked, and signal levers `828` and `830` turned left:

```text
{channel}/Corporal/controls     CHART 85973c19 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)
{channel}/Corporal/indications  CHART 85973c19 829NWK (829RWK) (827WLK) (827WLQK) 828WGK (828EGK) (828TEK) 830WGK (830EGK) (830TEK) (1MCK) (827T1K) (829T1K)
```

---

## 5. MQTT Transport Guarantees

1. **Replay protection for controls.** The contract says a field never acts on a control sent before its own start, and an old CODE press must never arrive after a newer one. On MQTT this holds by four rules together:
   - **Never retained.** Offices publish `{station}/controls` with the retain flag clear.
   - **Field drops retained deliveries.** A field discards, unread, any control message delivered with the retain flag set (a misconfigured office or a broker bridge could still retain one).
   - **Clean start.** The field's session that subscribes to `{station}/controls` uses a clean start (MQTT 5 `Clean Start = 1` with `Session Expiry = 0`; MQTT 3.1.1 `CleanSession = 1`), so the broker queues nothing for it while it is away.
   - **QoS 0, and no offline queue at the office.** Controls are published and subscribed at QoS 0, and an office client must not buffer controls while it is disconnected (turn off the client library's offline queue). QoS 0 has no retransmission, so no copy of a CODE press can be delivered after a later one.

   *Why QoS 0 and not QoS 1 with a sequence number:* QoS 1 is at-least-once. An office that reconnects with a persistent session retransmits an unacknowledged PUBLISH, which can then arrive after a newer frame. Preventing that needs a monotonic sequence term and stored high-water marks in the field, which adds state that must survive a restart. QoS 0 is correct with no state.

   *What is lost:* a CODE press can be lost when the link drops at that moment. The office detects it: the field republishes its indication frame after **every** control frame (rule 4). An office that sees no indication frame within a timeout after CODE shows "no response", and the dispatcher presses CODE again. That is the prototype's own remedy for a lost code.
2. **Retained indications, one per station.** The broker holds each station's latest frame, so an office or a listen-only repeater joining late is current at once.
3. **LWT → unavailable.** Each field client registers an LWT that publishes retained `UNAVAILABLE` on each of its stations' `indications` topics. An abrupt power or network loss therefore shows at every office.
4. **Republish on every control frame.** After every control frame, accepted or rejected, the field republishes its indication frame, so the office always gets an answer.
5. **One writer per topic.** Exactly one field serves a station's topics, and exactly one office publishes its `controls` ([**Code Line Interface §1**](../codeline/README.CodeLine.md#1-the-code-line-is-a-reflection-of-the-panel)). Listen-only repeaters subscribe to `indications` and never publish.

---

## 6. Donor Differences

| Donor | Was | Here |
| :--- | :--- | :--- |
| FieldUnit `AarTextCodec` | Terms separated by `, ` | Separated by a space, as in the Walkthrough and the appliance binding. |
| FieldUnit `AarTextCodec` | Signal order `SGK NGK TEK` / `SGS NGS HS`; signal side spelled `N` (left) / `S` (right) | Order `LEFT RIGHT STOP/TIME`; side spelled by its timetable letter (§4). |
| FieldUnit `cTcMachine::buildCodec`, the virtual plant, and the desk sketch | The token order (the wire schema) built by hand in each | Order comes from one deriver's chart (Code Line Interface §7.4). |
| FieldUnit `cTcMachine::buildCodec` | Switches, track circuits, signals, maintainer calls | Switches, locks, signals, maintainer calls, then model-board lamps. |
| FieldUnit ADR 0001 | Retained `json` schema topic, `telemetry`, `info` | Dropped; chart identity in every frame. |
| All donors | No version check; positional decode "out of sync" only when a token name differs | `CHART <id>` checked first. |

---

## 7. Other Encodings

A **US&S 506** encoding carries the same charts differently, and is not specified here:

- A station is one house, addressed by its authored panel-column number rather than by a topic.
- A code cycle has 16 steps, 7 of them function steps per direction, so a chart must fit its functions into 7 controls and 7 indications per station. One step may carry a whole group (`LEFT` / `RIGHT` / `TIME` on two steps, both long = `TIME`), and a station's code chart may drop functions (an unlamped track circuit) with a report.
- There is no token text and no room for a chart identity on the line; mismatch is caught where the charts are generated, not on the wire.
- The functions, their meaning, and the transaction rules of the Code Line Interface are unchanged.
