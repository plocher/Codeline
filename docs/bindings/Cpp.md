[![Codeline](../images/codeline-logo.svg)](../../README.md)
[Studio](../Studio.md) · [Codeline](../Codeline.md) · **[Smart Appliances](../SmartAppliances.md)**  
↳ [Appliances](../appliances/README.Appliances.md) · [Clusters](../clusters/README.Clusters.md) · **▸ C++ Binding** · [MQTT Binding](MQTT.md) · *uPython Binding*
---
---
# Smart Appliance C++ In-Process Binding, Reference Core & Test Harness

This document defines the normative **embedded C++ binding** for the transport-agnostic [**Smart Appliance Capability Cluster Reference**](../clusters/README.Clusters.md) and [**Smart Appliance Catalog**](../appliances/README.Appliances.md), along with the shared **Reference Appliance Core + Pluggable Driver Port** implementation architecture and the composable **Glue / Optional MQTT Mirror** stack.

---

## 1. Why a First-Class C++ Binding Matters

When a **FieldUnit bungalow** (an ESP32 or Raspberry Pi under the layout with local `cpNode-IOX` I²C cards) or a **Physical CTC Machine** (reading local switch/lamp panels) runs both controller logic and its local Smart Appliances on the same MCU or host:

- Routing local I²C sensor reads or CTC lever positions out over Wi-Fi to an MQTT broker just to loop them right back into the same binary adds latency, wastes broker traffic, and turns a Wi-Fi reboot into an interlocking or CTC outage.
- Calling the **exact same Typed Async Logical API** in-process in C++ eliminates string parsing, heap allocation, and network dependency—while preserving 100% source compatibility when an appliance *is* moved across MQTT.

---

## 2. Mechanical Mapping: Logical API → Embedded C++

The C++ binding targets constrained microcontrollers (ESP32, RP2040/Pico, Arduino) as well as native host builds (Raspberry Pi, macOS/Linux Studio test runners):

- **No C++ exceptions, no RTTI, zero heap churn on the hot path**: All operational commands and state reads avoid `std::string`, `std::vector`, and `std::function` heap allocations. String identifiers, paths, and fault details pass as `std::string_view` / `const char*` backed by static or fixed-capacity buffers.
- **100% Asynchronous Transparency**: Every command method returns **`void`**, matching the one-way pub/sub semantics of both the [**Code Line Interface**](../Codeline.md#3-the-code-line-interface-vs-the-smart-appliance-interface) and the [**MQTT Binding**](MQTT.md). Callers never block for RPC completion and never depend on synchronous return codes that cannot exist over MQTT.
- **Cached Physical State + Observer Hooks**: Every appliance interface provides a non-blocking O(1) `state()` getter (returning the last confirmed physical state, initialized unproven/restrictive until first confirmation) *plus* lightweight observer registration (`setObserver(...)`) for `onStateChanged` and `onEvent` push notifications.
- **Cooperative Timing (`tick(uint32_t now_ms)`)**: All timed behavior (servo motion ramps, `HOLDOFF` make/dropout timers, simulated turnout transit, flasher cadence, `CYCLE` sweeps, and `IDENTIFY` timeouts) advances deterministically from caller-supplied `now_ms`.

### Type Projection Table

| Logical Type | Embedded C++ Type | Notes |
| :--- | :--- | :--- |
| `bool` | `bool` | Native boolean. |
| `uint8`, `uint16`, `uint32`, `int32`, `float32` | `uint8_t`, `uint16_t`, `uint32_t`, `int32_t`, `float` | `<cstdint>` fixed widths. |
| `percent` (`0..10000`) | `Percent100` (`uint16_t`, hundredths of a percent) | `1900` = `19.00%`, `10000` = `100.00%`; exact, no float drift. |
| `duration_ms` | `uint32_t` | Milliseconds (rollover-safe via `ElapsedMillis` driven by `now_ms`). |
| `hz` | `float` | Flasher cadence in Hz. |
| `string` *(bounded)* | `std::string_view` *(input)* / `FixedString<N>` *(storage)* | Zero heap allocation. |
| `endpoint_ref` | Typed pointer (`ISensor*`, `IApbPeer*`, `IMast*`) | `nullptr` = unbound; resolves directly in memory for co-located nodes. |
| `BitVector` | `struct BitVector { uint32_t bits; uint8_t count; };` | Up to 32 channels in one register; `count != lever.outputs` rejects atomically. |
| `ColorValue` | Tagged struct `{ ColorKind kind; ColorName name; uint16_t c0, c1, c2; }` | Covers named AREMA colors, `Rgb16`, and `Hsb16`. |
| `SetOp` | `enum class SetOp : uint8_t { Assign, Increment, Decrement };` | Typed attribute mutation operator. |
| Domain Enums | `enum class TurnoutCommand : uint8_t { Normal, Reverse, Unchanged };`<br>`enum class TurnoutState : uint8_t { Normal, Reverse, InTransit };`<br>`enum class SignalIndication : uint8_t { Stop, ..., Clear };` | Contradictory `(Normal + Reverse)` command is unrepresentable at compile time. |

### Tick-Driven Elapsed Timers (`TickClock` & `ElapsedMillis`)

Rather than calling Arduino's global `millis()` inside appliance state machines—which would couple cluster logic to wall-clock time and break fast deterministic testing—the C++ binding uses a **tick-driven `ElapsedMillis`** variation of the Arduino community's `elapsedMillis` operator-overload idiom:

```cpp
// Updated once per cooperative tick(now_ms); zero dependency on global millis()
struct TickClock {
  uint32_t now_ms = 0;
  void advanceTo(uint32_t ms) { now_ms = ms; }
};

// Rollover-safe elapsed timer (modulo-2^32 unsigned arithmetic; zero heap allocation)
class ElapsedMillis {
  const TickClock& clock_;
  uint32_t origin_ms_;
public:
  explicit ElapsedMillis(const TickClock& clk, uint32_t initial_elapsed = 0)
    : clock_(clk), origin_ms_(clk.now_ms - initial_elapsed) {}

  operator uint32_t() const               { return clock_.now_ms - origin_ms_; }
  ElapsedMillis& operator=(uint32_t val)  { origin_ms_ = clock_.now_ms - val; return *this; }
  ElapsedMillis& operator-=(uint32_t val) { origin_ms_ += val; return *this; }
  ElapsedMillis& operator+=(uint32_t val) { origin_ms_ -= val; return *this; }
};
```

At the entry of `tick(uint32_t now_ms)`, an appliance core advances its `TickClock` (`clock_.advanceTo(now_ms)`). All internal timers (`transit_timer_`, `holdoff_timer_`, `flasher_timer_`, `cycle_timer_`, `identify_timer_`) then use familiar rollover-safe expressions—`transit_timer_ = 0;`, `if (transit_timer_ >= sim_transit_ms_)`, or drift-free `flasher_timer_ -= half_period_ms;`—while production firmware simply passes `millis()` into `tick()` and functional tests step `now_ms` across multi-second timeouts (and `uint32_t` wraparound boundaries) in microseconds.

*(Other target runtimes—such as a future **MicroPython** microcontroller binding or Python/TypeScript tooling inside [**Studio**](../Studio.md)—derive mechanically from the same logical API and can adopt the identical `TickClock` / `ElapsedMillis` pattern, e.g., backing `ElapsedMillis` with `time.ticks_diff()` on MicroPython to encapsulate its 30-bit tick wraparound.)*

---

## 3. Reference Appliance Core + Pluggable Driver Port

Roughly 90% of every Smart Appliance is **cluster state-machine logic** that must behave identically across Simulated, Direct Hardware, and Proxy backends—`OFFLINE`/`ONLINE` logical restart, `VITAL` write gating, `HOLDOFF` timers, `TURNOUT` feedback regimes and `SHORTED_CONTACTS` detection, `POSITION` ramps, `MAST` rulebooks, `CROSSING` gate delays, `CYCLE`, and `APB` tumble-down. Only ~10% touches physical pins or layout buses.

To prevent duplicating state machines across implementations, Layer 3 splits cleanly into two halves:

```text
  ┌─────────────────────────────────────────────────────────────────────┐
  │  Typed Async Logical API  (ITurnout, ISensor, IMast, IControl, ...) │
  ├─────────────────────────────────────────────────────────────────────┤
  │  Reference Appliance Core (TurnoutCore, SensorCore, MastCore, ...)  │  ← 100% shared state machines,
  │  Owns all cluster logic, attributes, NV config, & tick(now_ms)      │    invariants, & maintenance
  ├─────────────────────────────────────────────────────────────────────┤
  │  Narrow Driver Port       (ITurnoutDriver, ISensorDriver, ...)      │  ← minimal physical I/O hooks
  └───────────┬──────────────────────────┬──────────────────────────┬───┘
              ▼                          ▼                          ▼
     SimulatedDriver           DirectHardwareDriver          ProxyBusDriver
   (in-memory physics &       (cpNode-IOX, PCA9685,        (C/MRI, LocoNet,
    fault injection stub)      NeoPixels, GPIO)             LCC, JMRI)
```

### Example: `ITurnout`, `ITurnoutDriver`, and `TurnoutCore`

```cpp
enum class TurnoutCommand : uint8_t { Normal, Reverse, Unchanged };
enum class TurnoutState   : uint8_t { Normal, Reverse, InTransit };
enum class FaultType      : uint8_t {
  HwError, ConfigError, Overcurrent, Stall, ShortedContacts, VitalError, NonVitalError
};

struct TurnoutContacts { bool normal; bool reverse; };

struct ITurnoutObserver {
  virtual ~ITurnoutObserver() = default;
  virtual void onStateChanged(TurnoutState state) = 0;
  virtual void onEvent(const ApplianceEvent& ev)  = 0;  // Offline, Online, Fault, Config, ...
};

// Top surface: Typed Async Logical API (identical for Local, Mirror, and Remote MQTT)
struct ITurnout : public IMaintenance {
  virtual void         setAlignment(TurnoutCommand cmd) = 0;
  virtual TurnoutState state() const                    = 0;  // Non-blocking cached physical truth
  virtual void         setObserver(ITurnoutObserver* o) = 0;
  virtual void         tick(uint32_t now_ms)            = 0;
};

// Bottom surface: Minimal HW I/O Driver Port implemented per backend
struct ITurnoutDriver {
  virtual ~ITurnoutDriver() = default;
  virtual void            driveMotor(TurnoutCommand dir) = 0;  // Unchanged = de-energize
  virtual TurnoutContacts readContacts() const           = 0;
  virtual void            setIdentifyBeacon(bool on)     = 0;
};

// Reference Appliance Core: production state machine shared by Simulated, Direct, & Proxy
class TurnoutCore final : public ITurnout {
public:
  explicit TurnoutCore(ITurnoutDriver& driver, INvStore& nv);
  // Implements full Cluster:MAINTENANCE + Cluster:TURNOUT (+ optional POSITION/GROUPS)
  // over `driver`, guaranteeing identical behavior across all three realizations.
};
```

Because **`TurnoutCore`** is parameterized by **`ITurnoutDriver`**:
- **`SimulatedTurnoutDriver`** serves simultaneously as (1) the built-in **Simulator** in Studio, (2) the **reference boilerplate stub** showing every hook a new hardware or bus driver must implement, and (3) a **deterministic test & fault-injection fixture**.
- **`DirectTurnoutDriver`** binds the exact same `TurnoutCore` to `cpNode-IOX` (`MCP23017`) / `PCA9685` / Tortoise pins.
- **`ProxyTurnoutDriver`** binds the exact same `TurnoutCore` to a `C/MRI` `OB[]`/`IB[]` bit pair, `LocoNet` switch opcode, or `LCC` event pair—giving legacy 1-bit turnouts full `SIMULATED` transit correspondence and `OFFLINE` maintenance for free.

---

## 4. Composable Glue Stack: Local, Mirror & Remote MQTT

Because the [**MQTT Binding**](MQTT.md) is a purely mechanical codec wrapping the C++ Logical API, the boundary between any controller (Vital Field Logic or CTC Office) and any Smart Appliance composes into a clean optional glue stack:

```text
  = Top Logical Interface (Vital Logic / CTC Office / Test Runner) =
  [  Glue / Binding                                                ]
  [  MQTT PUBLISH    (optional)                                    ]
  [  MQTT SUBSCRIBE  (optional)                                    ]
  [  Glue / Binding                                                ]
  = Bottom Logical Appliance (Reference Core + Driver Port)        =
```

Studio and Codeline code generation instantiate one of three deployment modes without changing a single line of Vital Logic or Appliance Core code:

| Binding Mode | Operational Path (`cmd` ↔ `state`) | Management & Visibility (`maint` / `event`) | Best Used For |
| :--- | :--- | :--- | :--- |
| **1. Local-Only (In-Process)** | Direct C++ call (`turnout.setAlignment(...)`, `turnout.state()`) in RAM. Zero MQTT traffic. | Local C++ calls only. | Private internal I/O with no external observers (e.g., sealed standalone CTC panel or air-gapped bungalow). |
| **2. Local + MQTT Mirror** *(Recommended for co-located hardware)* | Direct C++ call in RAM — **Wi-Fi or broker outages never interrupt local vital logic or CTC panels**. | Optional `MqttMirrorAdapter` hooks `onStateChanged`/`onEvent` to **publish** `/state` and `/event`, and **subscribes** to `/maint` and `{channel}/all`. | Bungalows with local `cpNode-IOX` I²C cards and physical CTC machines that still want live [**Studio**](../Studio.md) mimic display, `EXPORT CONFIG` discovery, and `OFFLINE` servo/lamp calibration. |
| **3. Remote MQTT** | `MqttTurnoutProxy` *(caller side)* ↔ Broker ↔ `MqttTurnoutAdapter` *(appliance side)*. | Full `/maint`, `{channel}/all`, `/event`, and broker LWT → `UnavailableEvent`. | Office-hosted Vital Logic driving remote Wi-Fi microcontrollers, legacy Proxy hosts, or decoupled CTC panels. |

```cpp
// Example: Co-located bungalow turnout with Local + MQTT Mirror
DirectTurnoutDriver  hw_829(iox0, /*motor*/ 0, /*norm*/ 2, /*rev*/ 3);
TurnoutCore          turnout_829(hw_829, nv_store);

// Vital Field Logic talks directly to turnout_829 in RAM (immune to Wi-Fi outages)
cp_corporal.bindTurnout(829, turnout_829);

// Optional MQTT Mirror exposes /state, /event, and /maint to Studio without entering the vital loop
MqttTurnoutMirror    mirror_829(mqtt_client, "spcoast/turnout/829", turnout_829);
```

---

## 5. Deterministic Functional Test Framework & Transport Parity

Pairing the **Typed Async Logical API** with **`SimulatedDriver`** and cooperative **`tick(now_ms)`** makes comprehensive functional testing trivial and instantaneous (zero wall-clock `sleep()`):

1. **Top-Side API Exercises**: A single C++ test suite drives every cluster command (`setAlignment`, `setIndication`, `setLit`, `setLamps`, `offline`, `setAttribute`, `cycle`, `online`, `exportConfig`) and asserts exact `state()`, `onStateChanged`, and `onEvent` timelines.
2. **Bottom-Side Physics & Fault Injection**: `SimulatedDriver` exposes direct physical controls to the test harness—opening/closing point contacts, injecting `SHORTED_CONTACTS`, stalling a servo (`FAULT STALL`), shunting a track detector (`ACTIVE`) across inter-car gaps to verify `holdoff.dropout_ms`, burning out a signal lamp, or simulating a dropped transport (`UnavailableEvent`).
3. **Zero-Drift C++ / MQTT Parity Runs**: Because `MqttClientProxy` and `MqttApplianceAdapter` implement the exact same `ITurnout` / `IMast` / `ISensor` interfaces, the **identical functional test suite** runs twice:
   - **Direct Pass**: `TestSuite → TurnoutCore(SimulatedTurnoutDriver)` *(verifies all cluster state machines & invariants)*
   - **MQTT Loopback Pass**: `TestSuite → MqttTurnoutProxy → InMemoryMockBroker → MqttTurnoutAdapter → TurnoutCore(SimulatedTurnoutDriver)` *(verifies 1:1 codec serialization, positive-control rejection, retention, LWT `UnavailableEvent`, and `EXPORT CONFIG` discovery)*
