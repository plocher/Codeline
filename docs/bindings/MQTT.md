[![Codeline](../images/codeline-logo.svg)](../../README.md)
[Studio](../Studio.md) · [Codeline](../Codeline.md) · **[Smart Appliances](../SmartAppliances.md)**  
↳ [Appliances](../appliances/README.Appliances.md) · [Clusters](../clusters/README.Clusters.md) · [C++ Binding](Cpp.md) · **▸ MQTT Binding** · [Building](../BuildingAppliances.md) · *uPython Binding*
---
---
# Smart Appliance MQTT Transport &<BR>&nbsp;Topic Wire Encoding Binding

This document defines the normative, **mechanical projection** of the transport-agnostic [**Smart Appliance Capability Cluster Reference**](../clusters/README.Clusters.md) and [**Smart Appliance Catalog**](../appliances/README.Appliances.md) onto **MQTT** using plain-English `TOKEN` / `(TOKEN)` text payloads.

It follows the same **Logical API → Transport Binding → Wire Encoding** pattern used on the [**Code Line Interface**](../Codeline.md#3-the-code-line-interface-vs-the-smart-appliance-interface): every cluster command, state, event, attribute, and `endpoint_ref` maps deterministically onto MQTT topics and text frames with no appliance-specific protocol exceptions.

---

## 1. Endpoint Addressing & Two-Plane, Four-Topic Layout

Every Smart Appliance instance is uniquely addressed by its **channel**, logical **role** and instance **id**:

```text
{appliance} = {channel}/{role}/{id}
```

### Legend
- **{channel}**: A unique per-layout MQTT Topic prefix (`/layout/SPCoast/appliances`)
- **{role}**: The logical Appliance Role: `turnout`, `sensor`, `signalmast`, `trainorder`, `crossing`, `control`, `lamp`
- **{id}**: The name of the appliance on the layout (`835` for a switch or `829EA` for a signal mast)
- **`↦`**: Appliance **subscribes** to this MQTT topic (Controller → Appliance).
- **`⇤`**: Appliance **publishes** to this MQTT topic (Appliance → Controller).
- **`🅁`**: MQTT topic is **Retained** by the broker and requires `Last Will and Testament` (LWT) handling.

### Mechanical Projection of Logical Model → MQTT Topics

| Logical Surface | Plane | MQTT Topic | Dir | Retained | Semantics |
| :--- | :--- | :--- | :---: | :---: | :--- |
| Operational **COMMANDS** | Operational | `{appliance}/cmd` | `↦` | No | Unretained one-way command intent (never retained so waking nodes never actuate stale commands). |
| Operational **STATE** | Operational | `{appliance}/state` | `⇤` | **`🅁`** | **Sole retained operational topic** per appliance; always reports honest physical state (broker LWT publishes retained `UNAVAILABLE` here on link loss). |
| `Cluster:MAINTENANCE` **COMMANDS** | Management | `{appliance}/maint` | `↦` | No | Point-to-point management commands (only scope permitted to mutate attributes, run `CYCLE`, or toggle `OFFLINE`/`ONLINE`). |
| Broadcast & `Cluster:GROUPS` **COMMANDS** | Management | `{channel}/all` | `↦` | No | Single static multicast topic carrying only curated commands with an optional group prefix (`[GROUP <slug>] <COMMAND>`); **no arbitrary `SET`/`CLEAR` writes or `OFFLINE`/`ONLINE` transitions**. |
| Discrete **EVENTS** | Management | `{appliance}/event` | `⇤` | No | Unretained `<NOUN> <payload>` notifications (`FAULT`, `OFFLINE`, `ONLINE`, `ATTRIBUTES`, `CONFIG`, `BTN<k>`, `STICK`, `TEST_KEY`). |
| `Cluster:APB` Peer **STATE** | Peer Link | `{appliance}/apb` | `⇤` / `↦` | **`🅁`** | Outbound `HOME` / `DISTANT` line state published by autonomous APB masts and subscribed via `apb.next_signal`. |
| Shared-Node Liveness *(multi-appliance nodes)* | Management | `{channel}/node/{node-id}/state` | `⇤` | **`🅁`** | Retained `AVAILABLE` on connect; broker LWT (and clean shutdown) publishes retained `UNAVAILABLE` for microcontrollers/proxies hosting multiple appliances on one TCP session. |

### Appliance Pub/Sub Contract *(Static Subscriptions)*

Every MQTT-bound Smart Appliance maintains a fixed, minimal topic footprint (no dynamic broker subscribe/unsubscribe churn when `groups.members` changes):

- **Appliance subscribes to** *(Controller → Appliance)*:
  - `{appliance}/cmd` *(omitted on read-only appliances such as `SENSOR`)*
  - `{appliance}/maint` *(point-to-point management)*
  - `{channel}/all` *(curated layout broadcast & local `groups.members` filtering)*
  - `{apb.next_signal}/apb` *(only if `Cluster:APB` is composed)*
- **Appliance publishes to** *(Appliance → Controller)*:
  - `{appliance}/state` *(retained, with broker LWT)*
  - `{appliance}/event` *(unretained)*
  - `{appliance}/apb` *(retained, only if `Cluster:APB` is composed)*

*(Direction and retention are normative from this section and omitted in payload tables below.)*

---

## 2. Mechanical Type-to-Wire Encoding (`TOKEN` / `(TOKEN)` Codec)

The text codec encodes all logical types into human-readable ASCII frames suitable for both embedded MCUs and live `mosquitto_sub` troubleshooting.

### Positive-Control Token Convention
- **`TOKEN`** asserts a boolean condition (`true` / `ON` / `ACTIVE`).
- **`(TOKEN)`** negates it (`false` / `OFF` / inactive).
- Complementary pairs (such as `NORMAL (REVERSE)` / `(NORMAL) REVERSE`) travel in one atomic payload so proxies and brokers cannot reorder halves of a state pair.

### Type Mapping Table

| Logical Type / Construct | Wire Encoding | Parsing & Validation Rules |
| :--- | :--- | :--- |
| `bool` *(operational)* | `TOKEN` (`true`) \| `(TOKEN)` (`false`) | Domain token specified per cluster (`ON`/`(ON)`, `ACTIVE`/`(ACTIVE)`, `LIT`/`(LIT)`). |
| `bool` *(attributes)* | `true` \| `false` | Case-sensitive lowercase in `SET` / `CONFIG` / `ATTRIBUTES`. |
| `percent` (`0..10000` hundredths) | Decimal `0.00`..`100.00` (e.g., `19.00`, `72.50`) | Two fractional digits (or integer `0..100` for `dimming.brightness`). |
| `duration_ms`, `uint8..32`, `hz` | Base-10 integer / decimal (`1500`, `1.0`) | Out-of-range values are rejected at the codec boundary. |
| `endpoint_ref` | Full MQTT topic path (e.g., `{channel}/sensor/832T/state`, `{channel}/signalmast/830WAB/apb`) | Empty string `""` encodes `null`. |
| `SetOp` | `=` (`ASSIGN`), `+=` (`INCREMENT`), `-=` (`DECREMENT`) | Used in `/maint SET <path> <op> <val>`. |
| `ColorValue` | `ColorName` (`RED`, `YELLOW`, `GREEN`, `LUNAR`, `AMBER`, `WHITE`, `BLUE`, `PURPLE`, `PLUM`, `NONE`) or 16-bit tuple `($R,$G,$B)` / `HSB($H,$S,$B)` | Hex (`0xFFFF`) or decimal 16-bit channel components (no internal spaces inside tuples). |
| `BitVector` (`Cluster:LEVER`) | Complete ordered space-separated vector `1..N`:<br>`SW1 (SW2) ... SW<N>` or `LAMP1 (LAMP2) ... LAMP<M>` | **All-or-nothing**: missing, extra, out-of-order, or unrecognized tokens reject the entire frame. |
| `TurnoutCommand` | `NORMAL (REVERSE)` (`NORMAL`)<br>`(NORMAL) REVERSE` (`REVERSE`)<br>`(NORMAL) (REVERSE)` (`UNCHANGED`) | **`NORMAL REVERSE` (both asserted) is rejected as invalid** before reaching the logical API. |
| `TurnoutState` | `NORMAL (REVERSE)` (`NORMAL`)<br>`(NORMAL) REVERSE` (`REVERSE`)<br>`(NORMAL) (REVERSE)` (`IN_TRANSIT`) | Contradictory hardware contacts emit `/state (NORMAL) (REVERSE)` plus `/event FAULT SHORTED_CONTACTS`. |
| `SignalIndication` + `lit` | `<INDICATION>` *(when `lit = true`)*<br>`<INDICATION> (LIT)` *(when `lit = false`)*<br>`LIT` / `(LIT)` *(for `setLit(bool)` alone)* | Valid `<INDICATION>` words: `STOP`, `RESTRICTING`, `DIVERGING_RESTRICTING`, `APPROACH`, `DIVERGING_APPROACH`, `ADVANCE_APPROACH`, `APPROACH_MEDIUM`, `DIVERGING_CLEAR`, `CLEAR`. `DARK` in layout project files maps to `(LIT)` (`lit = false`), never an indication word. |
| `ApbLineState` | `HOME DISTANT` \| `HOME (DISTANT)` \| `(HOME) (DISTANT)` | `(HOME) (DISTANT)` is the vital fail-safe line state. |
| `CrossingState` | `FLASHING GATES_DOWN` \| `FLASHING (GATES_DOWN)` \| `(FLASHING) (GATES_DOWN)` | Reflects physical flasher and gate-down proof. |
| `ConfigMap` / `AttributeMap` | Space-separated `path=value` entries following `CONFIG`, `ATTRIBUTES`, or `IMPORT` | Keys use canonical `[<component>.]<cluster>.<attribute>` paths. Values contain no internal spaces (maps flatten to dotted paths like `position.presets.NORMAL=19.00`; lists are comma-separated like `groups.members=ctc-panel,corporal` or empty for `[]`). |

---

## 3. Cluster-by-Cluster Wire Projection

### Management Plane (`{appliance}/maint` & `{appliance}/event` — Point-to-Point Only)

| Logical Command / Event | Topic | Wire Frame |
| :--- | :--- | :--- |
| `identify(ON \| OFF \| PULSE, ms)` | `/maint` | `IDENTIFY ON` \| `IDENTIFY OFF` \| `IDENTIFY <ms>` |
| `exportAttributes()` / `exportConfig()` | `/maint` | `EXPORT ATTRIBUTES` \| `EXPORT CONFIG` |
| `offline()` / `online()` | `/maint` | `OFFLINE` \| `ONLINE` |
| `setAttribute(path, op, val)` | `/maint` | `SET [<comp>.]<cluster>.<attr> [= \| += \| -=] <val>` |
| `clearAttribute(path)` / `clearFault()` | `/maint` | `CLEAR [<comp>.]<cluster>.<attr>` \| `CLEAR FAULT` |
| `importConfig(config)` | `/maint` | `IMPORT <config>` *(atomic: any unknown, non-`NV config`, or out-of-range entry rejects the whole frame)* |
| `cycle(count, dwell_ms)` | `/maint` | `CYCLE <count> <dwell_ms>` |
| `FaultEvent(type, detail?)` | `/event` | `FAULT <TYPE> [(description)]` |
| `OfflineEvent` / `OnlineEvent` | `/event` | `OFFLINE` \| `ONLINE` |
| `AttributesEvent(data)` / `ConfigEvent(data)` | `/event` | `ATTRIBUTES <data>` \| `CONFIG <data>` |

### Curated Broadcast & Group Multicast (`{channel}/all`)

To prevent accidental layout-wide outages (such as broadcasting `OFFLINE`, `CYCLE`, or overwriting per-device calibration attributes via `SET`), `{channel}/all` uses a **closed, domain-namespaced command grammar** handled separately from `{appliance}/maint`, with an optional `GROUP <slug>` prefix:

```text
{channel}/all  ←  [GROUP <slug>] <COMMAND>
```

If `GROUP <slug>` is present, an appliance executes `<COMMAND>` only when `<slug>` is in its local `groups.members` list; if omitted, the command applies layout-wide. No arbitrary `SET`/`CLEAR` attribute writes and no `OFFLINE`/`ONLINE`/`IMPORT`/`CLEAR FAULT` lifecycle verbs exist on `{channel}/all`—any unrecognized frame is dropped by the parser, and appliances that do not compose the target domain silently ignore it.

| Curated `<COMMAND>` | Example (`[GROUP <slug>] <COMMAND>`) | Behavior |
| :--- | :--- | :--- |
| `EXPORT CONFIG` | `EXPORT CONFIG`<br>`GROUP corporal EXPORT CONFIG` | Live appliances reply on their `/event` with `CONFIG <data>` after `0–250 ms` random jitter (zero-zombie discovery). |
| `IDENTIFY ON \| OFF \| <ms>` | `IDENTIFY OFF`<br>`GROUP corporal IDENTIFY 5000` | Activates, silences, or pulses physical locate beacons (`IDENTIFY OFF` layout-wide silences every forgotten beacon). |
| `LIGHT DIM <0..100>` | `GROUP ctc-panel LIGHT DIM 45`<br>`LIGHT DIM 80` | Smoothly dims `DIMMING`-capable fixtures and panels using each device's own `dimming.fade_ms` without altering per-device calibration. |
| `LIGHT ON \| OFF` | `GROUP room-lights LIGHT OFF` | Toggles `Appliance:LAMP` fixtures *(never extinguishes wayside signal masts or CTC indication jewels)*. |
| `LIGHT COLOR <ColorValue>` | `GROUP room-lights LIGHT COLOR AMBER` | Sets active color on `COLOR`-capable `Appliance:LAMP` fixtures *(never overrides wayside signal aspects or per-device `color.palette`)*. |

*(The `[GROUP <slug>] <DOMAIN> <VERB> [args]` namespace remains cleanly extensible for future curated domain commands—such as `CROSSING MUTE`/`UNMUTE` or `MAST` commands subject to their own safety gates—without ever exposing raw attribute mutation on `{channel}/all`.)*

### Operational & Domain Clusters

| Cluster | `/cmd` | `/state` | `/event` |
| :--- | :--- | :--- | :--- |
| `ON/OFF` *(root `LAMP`)* | `ON` \| `(ON)` | `ON` \| `(ON)` | — |
| `POSITION` | *(via parent cluster)* | *(via parent cluster)* | `FAULT OVERCURRENT [(desc)]`<br>`FAULT STALL [(desc)]` |
| `LEVER` | `LAMP1`\|`(LAMP1)` `...` `LAMP<M>`\|`(LAMP<M>)` | `SW1`\|`(SW1)` `...` `SW<N>`\|`(SW<N>)` | — |
| `BUTTON` | — | — | `BTN<k>` \| `(BTN<k>)` |
| `TURNOUT` | `NORMAL (REVERSE)` \| `(NORMAL) REVERSE` \| `(NORMAL) (REVERSE)` | `NORMAL (REVERSE)` \| `(NORMAL) REVERSE` \| `(NORMAL) (REVERSE)` | `FAULT SHORTED_CONTACTS [(desc)]` |
| `SENSOR` | — | `ACTIVE` \| `(ACTIVE)` | — |
| `MAST` | `<INDICATION> [LIT\|(LIT)]` \| `LIT` \| `(LIT)` | `<INDICATION>` \| `<INDICATION> (LIT)` | — |
| `APB` | *(subscribes to `apb.next_signal` `/apb`)* | `/apb`: `HOME DISTANT` \| `HOME (DISTANT)` \| `(HOME) (DISTANT)` | `STICK SET` \| `STICK CLEAR` |
| `CROSSING` | `ACTIVE` \| `(ACTIVE)` | `FLASHING GATES_DOWN` \| `FLASHING (GATES_DOWN)` \| `(FLASHING) (GATES_DOWN)` | `TEST_KEY` |

---

## 4. MQTT Transport Guarantees: Safety, Discovery, LWT & Groups

1. **No Retained Commands**: `/cmd`, `/maint`, and `{channel}/all` are **never retained**. Combined with `maintenance.post = false` on `TURNOUT`, reconnecting or booting microcontrollers never throw switches unexpectedly before occupancy detectors stabilize.
2. **Blast-Radius Containment on `{channel}/all`**: Layout-wide and group fan-out are restricted to the closed, curated command set above—never `OFFLINE`, `ONLINE`, `IMPORT`, `CLEAR FAULT`, or raw `SET`/`CLEAR` attribute writes—so a single broadcast message can never take the railroad offline or corrupt per-device calibration.
3. **No Retained Configuration (Zero-Zombie Discovery)**: Configuration is never retained on the broker. Controllers and [**Studio**](../Studio.md) discover live hardware by subscribing to `{channel}/+/+/event` and publishing `EXPORT CONFIG` on `{channel}/all`. Every live appliance waits a **random `0–250 ms` jitter** before publishing its unretained `/event CONFIG <data>`, preventing broker packet storms. Newly powered boards also emit a single boot-time `/event CONFIG <data>` for hot-plug arrival.
4. **Broker LWT → Retained `UNAVAILABLE` (Per-Appliance and Shared-Node)**:
   - **Dedicated per-appliance client** (`maintenance.node == ""`): registers a broker LWT publishing retained `UNAVAILABLE` on `{appliance}/state` (mirroring [**Code Line MQTT Binding §1**](CodeLineMQTT.md#1-topic-scheme)). An abrupt disconnect surfaces `UnavailableEvent` immediately to live and late-joining controllers for executive vital fail-safe substitution (see [**Codeline: Executive-Owned Vital Fail-Safe Substitution**](../Codeline.md#engineering-principles)); on reconnect, the appliance republishes its live `/state`.
   - **Shared-socket multi-appliance node** *(microcontrollers or proxy gateways hosting multiple appliances over one TCP session, implemented when the first group-hosted hardware arrives)*: the node publishes retained `AVAILABLE` on `{channel}/node/{node-id}/state` upon connecting and registers a single list-free LWT publishing retained `UNAVAILABLE` on that same topic (and publishes `UNAVAILABLE` on clean shutdown). Each hosted appliance reports its hosting `{node-id}` in `maintenance.node` (`AttributesEvent`). Controllers subscribe to `{channel}/node/+/state`; whenever a node reports `UNAVAILABLE`, the controller immediately marks every appliance whose `maintenance.node` matches `{node-id}` as **unproven** (applying vital fail-safe substitution) and re-runs `/maint EXPORT ATTRIBUTES`. Dead appliances never answer and stay unproven; an appliance moved to another node answers from its new node and returns to healthy in one round trip with no member-list cleanup or payload-size limit.
   - **Controller reconnect**: Whenever a controller's own broker session drops and reconnects, it marks all bound appliances **unproven** and re-runs `EXPORT ATTRIBUTES` before re-admitting them.
5. **In-Place Logical Restart on `ONLINE` & Health Proof (`EXPORT ATTRIBUTES`)**: Receiving `/maint ONLINE` executes [`RESTART BEHAVIOR`](../clusters/README.Clusters.md#restart-behavior-power-on-and-exiting-offline) without disconnecting the MQTT session or firing the LWT—reloading `NV config` (including `groups.members`), entering Startup State, and publishing `/state`, `/event CONFIG`, and `/event ONLINE` *(or `/event CONFIG` and `/event ONLINE` followed by `/state` once moving heads finish reaching `STOP` on `SIGNALMAST`)*. Because `/event` is unretained, controllers cannot learn pre-existing `OFFLINE` or `FAULT` states from retained `/state` alone: at controller startup, after `UNAVAILABLE` recovery, and after `/event ONLINE`, a controller holds the appliance unproven (fail-safe substituted) until it sends `/maint EXPORT ATTRIBUTES` and receives `/event ATTRIBUTES` confirming `maintenance.offline=false` and `maintenance.faulted=false` (see [**Controller Health Proof & Re-Admission**](../clusters/README.Clusters.md#controller-health-proof--re-admission)).
6. **Decentralized `Cluster:GROUPS` Lifecycle (Static `{channel}/all` Subscription)**:
   ```text
   1. Provision membership (point-to-point while OFFLINE):
      {channel}/control/Corporal/maint   ──▶ SET groups.members+=ctc-panel
      {channel}/control/Sargent/maint    ──▶ SET groups.members+=ctc-panel
      (Each column saves "ctc-panel" to NV flash; broker subscriptions do not change.)

   2. Operate group (live, while ONLINE, via curated domain command):
      {channel}/all                      ──▶ GROUP ctc-panel LIGHT DIM 45
      (Every appliance on {channel}/all checks its local groups.members list in O(1);
       CTC columns dim to 45% in unison while non-members drop the frame.)

   3. Dissolve membership (point-to-point while OFFLINE):
      {channel}/control/Corporal/maint   ──▶ SET groups.members-=ctc-panel
      (Column removes "ctc-panel" from NV flash and stops matching that slug.)
   ```

---

## 5. Concrete `CONTROL` Wire Examples

### (a) Electric Switch Lock (`{channel}/control/lock:827`)
```text
# NV config
lever.inputs      = 1
lever.outputs     = 1
lever.input_style = "MAINTAINED"
button.inputs     = 1
button.mode       = "LEVEL"
groups.members    = ["corporal", "wayside-locks"]

# Sample MQTT wire traffic
/state  (SW1)     # Lock closed (normal/power)
/event  BTN1      # Crew presses and holds unlock request
/cmd    LAMP1     # Controller illuminates release indicator
/event  (BTN1)    # Crew releases button
/state  SW1       # Crew opens lock / moves selector to HAND
```

### (b) CTC Machine Panel Column (`{channel}/control/Corporal`)
```text
# NV config
lever.inputs      = 6
lever.outputs     = 9
lever.input_style = "MAINTAINED"
button.inputs     = 1
button.mode       = "ONESHOT_RELEASE"
groups.members    = ["ctc-panel"]

# Sample MQTT wire traffic
/state         SW1 (SW2) (SW3) SW4 SW5 (SW6)                  # Levers positioned
/event         BTN1                                           # CODE button released
/cmd           LAMP1 (LAMP2) (LAMP3) LAMP4 LAMP5 (LAMP6) (LAMP7) (LAMP8) (LAMP9)
{channel}/all  GROUP ctc-panel LIGHT DIM 60                   # Panel group dimmed to 60%
```
