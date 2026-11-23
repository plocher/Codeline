[![Codeline](../images/codeline-logo.svg)](../../README.md)

[Studio](../Studio.md) · **[Codeline](../Codeline.md)** · [Smart Appliances](../SmartAppliances.md)  
↳ [Walkthrough](../Walkthrough.md) · [Roadmap](../Roadmap.md) · **▸ Research: Engine Closed-Loop Spike**

---

# M1c Finding: Can the FieldUnit Engine Run Over the Smart Appliance Interface?

**Verdict: adapt it, with named changes. Do not rewrite below the control table.** The FieldUnit engine keeps its vital state (locks, correspondence, track circuits, the signal stick) separate from its I/O, and it compares physical feedback against its own demand. The changes it needs are therefore small and local:

- Replace the whole driver layer with an appliance binding.
- Stop `Switch` from inventing `MOVING` from its own command.
- Split each mast's demanded indication from its confirmed indication.
- Start time locking, and the vacant-approach release, only when the mast is proven at `STOP`.
- Hold switch locks while any mast lined over them is not proven at `STOP`.

The spike did all of this in about 270 added lines across seven headers. With those changes, all 21 existing test programs and five new closed-loop scenarios pass through stand-in appliances that have latency, travel time, and health faults. Nothing above `InterlockingEngine::evaluate()` changed apart from two small terms.

The spike also found engine defects that have nothing to do with the boundary, and M2 must fix them: opposing signals are not locked against each other, and approach locking applies only when the dispatcher cancels. See [Engine defects found on the way](#5-engine-defects-found-on-the-way).

---

## 1. How the Spike Was Run

- **Donor:** `FieldUnit` at commit `6d18a79` (branch `spike/corporal-generator`). The spike copied `src/`, `tests/` and `examples/` (not `.git`) into a session scratch directory and changed only that copy. The donor repository was not touched. (Roadmap M1c says "spike in the donor repo"; the copy was used instead because the donor is under active work.)
- **Build:** each test is a standalone program, `clang++ -std=c++17 -I src tests/<file>.cpp`, as in the donor's `AGENTS.md`.
- **Three modes**, chosen by compile flags, so every effect can be traced to its cause:

| Mode | Flags | Meaning |
| :--- | :--- | :--- |
| **A** | none | Donor behavior. The spike's edits are all behind `#ifdef`. |
| **B** | `FIELDUNIT_SAI` | All physical I/O goes through stand-in appliances, with fail-safe substitution in the engine. The engine's locking logic is unchanged. |
| **C** | `FIELDUNIT_SAI` + `FIELDUNIT_SAI_CONFIRMED` | B, plus: time locking and the approach release wait for a confirmed `STOP`, switch locks are held while a mast is not proven at `STOP`, and the indication vector reports the confirmed indication. |

**The stand-in appliances** (`src/appliance/StandIns.h`, 143 lines) follow the shape in [C++ Binding](../bindings/Cpp.md):

- `SimTurnout::setAlignment`, `SimMast::setIndication` and `SimSensor` take one-way `void` commands and have a cached `state()` that changes only inside their own `tick(now)`.
- Each has a health value: `OK`, `OFFLINE`, `FAULT` or `UNAVAILABLE`.
- Timing is configurable: command latency, turnout travel time (or "manual", where the test drives the contacts), mast confirmation time, and a jammed or stuck mode.
- A command sent while the appliance is `UNAVAILABLE` is lost.
- A mast that is moving toward a proceed indication reports `STOP`, as [`Appliance:SIGNALMAST`](../appliances/README.Appliances.md#appliancesignalmast) requires.

The plant gives every `Switch`, `TrackCircuit` and `SignalMast` its own stand-in when the item is added.

In modes B and C, a test's raw input calls (`TrackCircuit::update`, `Switch::updateFeedback`, `forceSettledPosition`) are redirected. They become *physics on the appliance* (a train shunts, a contact makes, a link drops). The engine learns about them only by sampling `state()` at the start of its next scan, so a test cannot reach past the boundary.

---

## 2. Where the Engine Touches the Physical World

Line numbers are for donor commit `6d18a79`.

### Inputs (what the engine reads)

| Input | Engine entry point | Who writes it today |
| :--- | :--- | :--- |
| Track occupancy | `TrackCircuit::update()` (`TrackCircuit.h:77`). The vital read is `isClear()` (`:41`): `VACANT` and quality `GOOD`. | `TrackCircuitDriver::sample` (`drivers/TrackCircuitDriver.h:22`), `MqttApplianceBus` (`drivers/MqttApplianceBus.h:145`), tests, `.ino` loops |
| Switch points | `Switch::updateFeedback()` (`Switch.h:224`) sets `reported_` | `SwitchDriver::sample` (`drivers/SwitchDriver.h:39–47`), `MockSwitchDriver::sample` (`drivers/DriverPolicy.h:58`), `MqttApplianceBus` (`:156`), tests |
| Mast state | **nothing**: the engine never reads a mast's physical state | — |

### Outputs (what the engine commands)

| Output | Engine state | Who reads it today |
| :--- | :--- | :--- |
| Switch demand | `Switch::commandedPosition()` | `SwitchDriver::drive` (`drivers/SwitchDriver.h:55`), `MqttApplianceBus::sync` (`:177`), `MockSwitchDriver::drive` |
| Mast indication | `SignalMast::setIndication()` (`SignalMast.h:102`) sets `currentIndication_` and computes the head aspects through `SignalAspectPolicy` | `SignalMastDriver`, `SemaphoreDriver` and `CplMastDriver` read `head1()`…`head3()` |

**Two ingress and egress paths exist.** `InterlockingPlant::sampleInputs()` and `driveOutputs()` (`drivers/DriverPolicy.h:83`, `:94`) run inside `tick()` and serve only the `DriverPolicy` path. The example sketches instead call per-appliance drivers around `executeCycle()` from `loop()`, outside `tick()`.

### Where the engine assumes its own command took effect

1. **`Switch::throwSelf()` sets `reported_ = MOVING`** (`Switch.h:245`). The engine invents the "points are moving" state from its own command. Under an asynchronous turnout the points may not have started, and the command may have been lost.
2. **`Switch` starts with `reported_ = NORMAL`** (`Switch.h:29`), and derails are forced to `NORMAL` at configuration. Before the first report, the engine believes the switch is in correspondence (scenario S5).
3. **`SignalMast::currentIndication()` is both the demand and the display.** `exportIndicationVector()` reports it to the office as the signal's indication (`InterlockingPlant.h:807`, `:815`).
4. **The time element starts when the cancel code arrives**, not when the mast shows `STOP`. `SignalControl::updateCommand()` sets `timeLockExpiryMs_ = nowMs + duration` (`SignalControl.h:115`). It is called from `applyControlTransaction()` (`InterlockingPlant.h:666`) before the next scan has even commanded the mast to `STOP`. Tracer-bullet Test 7 and CP Christopher Test 2 depend on this: they cancel, advance the clock 35–40 s, and expect the switches free on the first scan after.
5. **A cancel with the approach vacant releases at once** (`SignalControl.h:117`), whatever the mast is displaying.
6. **Route locks are dropped and rebuilt every scan.** `evaluateVitalRules()` removes `ROUTE_LOCKED` from every switch (`InterlockingPlant.h:721`), and `evaluate()` puts the lock back only on cleared or traversing routes. A switch is therefore free in the same scan that its signal is commanded to `STOP`, while the mast may still be displaying proceed.
7. **The office `GK` indication comes from the signal stick, not the mast.** `AarTextCodec` sets `SGK`/`NGK` from `activeAuthority` (`WireCodec.h:923`). The office lamp lights while the mast is at `STOP` behind a moving switch (scenario S2).

---

## 3. Evidence

### 3.1 Existing test suite

All counts are test programs; each program has many assertions.

| Mode | Unmodified `tests/` | Patched copies (`tests_sai/`) |
| :--- | :---: | :---: |
| Baseline (donor source, before any edit) | **21 / 21** | — |
| A (spike source, no flags) | 21 / 21 | 21 / 21 |
| B (boundary) | **12 / 21** | **21 / 21** |
| C (boundary + confirmed-`STOP` locking) | **11 / 21** | **21 / 21** |

These 11 tests pass in every mode with no edits: `automatic_signal`, `capacity_guards`, `christopher_sketch`, `corporal_sketch`, `ctc_machine`, `hardware_drivers`, `line_input`, `plant_serializer`, `train_order_face`, `universal_sketch` and `unresolved_references`. In mode B, `cpl_signaling` also passes unmodified.

Each failure in the unmodified suite stops at its *first* failed assertion. In every case the cause is the test's assumption about timing, not the vital logic:

| Failure kind | Tests | What the test assumes |
| :--- | :--- | :--- |
| Reads engine state before a scan | `centralized_chubb`, `console`, `mqtt_appliance_bus`, `wire_codec`; in C also `cpl_signaling` | An input written (or a mast set) is visible to the engine at once, without a scan |
| The command took effect | `tracer_bullet`, `cp_christopher`, `derail`, `driver_policy` | `reportedPosition() == MOVING` right after a throw |
| Test physics contradicts a command in flight | `sectional_release` | Contacts can be set to `NORMAL` while a `REVERSE` command is still on its way to the machine |
| The timer starts at the cancel (C only) | `tracer_bullet` T7, `cp_christopher` T2, T4 | Assumption 4 above |

To check that each scenario holds *after* its first failure, the spike made patched copies with 43 one-line edits in 10 files. Each edit is tagged `// M1c <KIND>`, and `patch_tests.py` generates them:

| Kind | Edits | What the edit does |
| :--- | :---: | :--- |
| `SAMPLE` | 16 | `cp.sampleInputs(t)` after a test writes switch feedback (ingress only, no evaluation) |
| `SCAN` | 10 | One `cp.tick()` before reading engine state that was just written |
| `KR` | 10 | `reportedPosition() == MOVING` becomes `KR() == false` |
| `PROVE` | 6 | One or two scans after a cancel, so the mast is commanded `STOP` and confirms it before the clock jumps (needed only in C) |
| `PHYS` | 1 | Lets a command reach the appliance before the test sets the contacts |

None of the edits changes an expected vital outcome: what clears, what is locked, what is rejected. The patched copies also pass 21/21 in mode A.

**One ordering change was needed in the boundary.** The `DriverPolicy` path wrote into the appliance after the engine had already sampled it. Ingress is now: legacy driver sample, then appliance tick, then engine sample. This only confirms that drivers belong *below* appliances.

### 3.2 New closed-loop scenarios

`tests_m1c/test_closed_loop_scenarios.cpp` uses one switch, an OS, an approach circuit in rear of the signal, and two routes. Checks report without aborting. `GAP` marks a check that records a known engine defect.

| Scenario | Mode B | Mode C |
| :--- | :--- | :--- |
| **S1** A mast that is slow to confirm `STOP` (3 s) is cancelled with a train on approach. Time locking must run 30 s from the *confirmed* `STOP`. | **FAIL**: the switch is freed at cancel + 30 s, while the mast confirmed `STOP` only at cancel + 3.2 s | **pass**: locked until confirmation + 30 s, and a throw at cancel + 30.6 s is refused |
| **S2** A jammed turnout: `REVERSE` is commanded and never arrives. | pass: no proceed command in 60 s; KR down; marked out of correspondence after 5 s; clears once the points arrive | pass |
| **S2b** A slow but healthy turnout (300 ms latency, 2 s travel) | pass: the first proceed command is in the scan that samples the confirmed `REVERSE` | pass |
| **S3a** A turnout goes `UNAVAILABLE` while its signal is `CLEAR` | pass: substituted out of correspondence; `STOP` commanded in the same scan and confirmed | pass |
| **S3b** A mast goes `UNAVAILABLE` while it shows `APPROACH` to a train, and is then cancelled | **FAIL**: the switch is freed at once while the mast may still be lit; the time lock expires with no proof of `STOP` | **pass**: locked indefinitely until the link returns; the engine then re-sends `STOP`, the mast confirms, and the 30 s count starts from that proof |
| **S3c** A route sensor beyond the OS goes `UNAVAILABLE` while `APPROACH` is displayed to a train | `STOP` commanded and confirmed; **GAP**: switch 1 is free | same **GAP** |
| **S4** The OS sensor is `UNAVAILABLE`, `FAULT` or `OFFLINE` | pass: reads occupied, the switch is detector-locked, a throw is refused and no command reaches the turnout | pass |
| **S4b** The OS sensor fails while the signal is `CLEAR` | pass: treated as a train entering (knockdown, stick dropped); after recovery the signal stays at `STOP` until recoded | pass |
| **S5** Startup with an `UNAVAILABLE` turnout | **GAP**: before the first scan the engine reports KR up (it assumes `NORMAL`); the first scan corrects it | same |
| **Totals** | 37 ok, **5 FAIL**, 3 GAP | **42 ok, 0 FAIL**, 3 GAP |

A probe (`tests_m1c/probe_opposing.cpp`) also showed that two opposing home signals over the same OS **both command `CLEAR`** in every mode.

---

## 4. Engine Changes Needed (for M2)

Sizes are from the spike: lines added behind `#ifdef`, with nothing removed. Production code should replace the old code rather than sit beside it.

| # | File, function | Change | Size |
| :-: | :--- | :--- | :--- |
| 1 | `drivers/*` (`DriverPolicy`, `IoBitDriverPolicy`, `MqttDriverPolicy`, `MockSwitchDriver`, `SwitchDriver`, `TrackCircuitDriver`, `SignalMastDriver`, `SemaphoreDriver`, `CplMastDriver`, `MqttApplianceBus`, `IOBus`); `InterlockingPlant::overrideDriver`, `mockSwitch`, `setDefaultDriverPolicy` | **Remove from the engine.** Their content becomes Smart Appliance drivers, cores and the MQTT binding (M1a, M2). `sampleInputs()` / `driveOutputs()` become appliance ingress and egress. | about −1 200 lines; +~30 |
| 2 | `Switch.h`: `throwSelf`, constructor, `updateFeedback`, `tick` | Do not set `MOVING` on a throw. Start `reported_` as `UNKNOWN`. Ingress: map `TurnoutState`, and substitute `OUT_OF_CORRESPONDENCE` when the appliance is not `OK`. Egress: send `setAlignment` when the demand changes; re-send after the appliance recovers only if `WLR()`. `updateFeedback` leaves the vital API. Travel supervision compares the report with the *demand*. | ~80 |
| 3 | `TrackCircuit.h`: `update` | Split the raw input from the engine sample. Ingress substitutes `OCCUPIED` with `LOST_COMMS` (`UNAVAILABLE`) or `DEFECT` (`FAULT`, `OFFLINE`). Decide whether staleness and `dropoutDelay` stay (see section 7). | ~40 |
| 4 | `SignalMast.h` | Keep `currentIndication()` as the demand. Add `confirmedIndication()` (`STOP` when unhealthy), `provenStop()` (*false* when unhealthy), and egress with a re-send while unconfirmed after recovery. **Move** `SignalAspectPolicy` and the head and marker computation below the seam into `MastCore`. | ~40, then remove the aspect code |
| 5 | `SignalControl.h`: `updateCommand`, `evaluate` | On a cancel, drop ASR but do not start the TER. New `stopProven(now)`: start the TER when the approach is occupied, or release when it is vacant. Use `ElapsedMillis`: `nowMs >= expiry` (`:155`) is not rollover-safe. | ~35 |
| 6 | `InterlockingPlant.h`: `evaluateVitalRules` step C, `exportIndicationVector`, `add*`, `tick` | Before stick evaluation, prove `STOP` on every mast of the cancelled direction's routes. Export the confirmed indication. Bind appliances by reference at configuration (not from an internal pool). Remove the `millis()` fallback (`:705`). Stop pairing `signals[i]` with `masts_[i]` by index (`:807`). | ~45 |
| 7 | `ControlTable.h`: `evaluateIndication`, `evaluate` | Add a term: an unhealthy governed mast evaluates to `STOP`. After the masts are set, hold `ROUTE_LOCKED` on the switches of every aligned route whose mast is not proven at `STOP`. | ~16 |
| 8 | `WireCodec.h` `encodeIndications` | Take `GK` from the confirmed mast indication, not from `activeAuthority`, once Codeline defines `GK` (section 6, item 5). | small |
| 9 | Tests | Apply the five idioms in section 3.1, or better, write scenarios against appliances, as in `tests_m1c/`. | 43 edits seen |

---

## 5. Engine Defects Found on the Way

These defects are not caused by the boundary, but they are vital. M2 must fix them before it claims the behavior in [Codeline §4](../Codeline.md#4-vital-field-logic-and-declarative-signal-resolution).

1. **No opposing-signal check (`RGKR`).** `applyControlTransaction()` and `evaluate()` never check that opposing signals are at `STOP`. Two home signals facing each other over the same OS both clear (probe). Codeline §4 step 1 requires this check. In the closed-loop design it must read the opposing masts' `provenStop()`.
2. **Approach locking applies only when the dispatcher cancels.** When a signal drops for any other reason (a route sensor fails, a home fact drops) with a train approaching, the route locks fall away in the same scan, and the switches are free in front of the train (S3c). The fail-safe substitution makes this much more frequent: any link blip on a route sensor now causes it.
3. **`Route::approaching()` has two meanings.** `approachContribution()` (`ControlTable.h:618`) uses the circuit to *reduce* the aspect, which is a block-ahead rule. `applyControlTransaction()` (`InterlockingPlant.h:661`) uses the same circuit as the approach-locking circuit *in rear* of the signal. In S1, a train on approach downgrades the signal to `APPROACH`.
4. **`IndicationVector.signals[i]` is paired with `masts_[i]` by declaration index** (`InterlockingPlant.h:807`), not through the routes.
5. **Timers are not rollover-safe** (`SignalControl.h:43`, `:155`).

---

## 6. What Codeline's Documents Leave Open or Get Wrong

1. **"`SIGNALMAST` → `STOP`" is right for one use and wrong for another.** When a mast is read as *what the next signal shows*, `STOP` is the restrictive answer. When it is read as *is this signal proven at stop* (the `RGKR` check, the start of the TER, the release of locks), `STOP` is the **permissive** answer. [Codeline: Executive-Owned Vital Fail-Safe Substitution](../Codeline.md#engineering-principles) should say that an unhealthy mast is `STOP` for indication uses and **not proven at `STOP`** for locking uses. The spike implements both (`confirmedIndication()`, `provenStop()`).
2. **A mast does nothing when it loses its controller.** The `MAST` cluster and `RESTART BEHAVIOR` define no behavior for a lost link to the controller. A mast that is `UNAVAILABLE` to vital logic may keep showing its last indication, including `CLEAR`, indefinitely; the substitution exists only inside the logic. Either the cluster needs a controller-loss watchdog (drop to `STOP` after N ms without a command or heartbeat), or the spec must say that vital logic holds locks until it gets proof, which is what the spike does (S3b).
3. **Re-admission is not defined.** [Walkthrough §2](../Walkthrough.md#2-offline-servo-calibration) says Codeline "re-admits" a turnout using its `/state`. It does not say whether the logic re-issues the last demand (a throw nobody just asked for) or adopts the physical position. It also does not say whether a signal may re-clear by itself when an appliance returns (S3a: it does, because `HSR` is still up). The spike re-sends the switch demand only when the switch is unlocked.
4. **Time locking is specified only for a dispatcher cancel** ([Codeline §4](../Codeline.md#4-vital-field-logic-and-declarative-signal-resolution), step 5). The docs do not say three things: that the vacant-approach release must also wait for proof of `STOP`; what happens when a signal drops in front of a train for another reason (section 5, item 2); and what happens when proof never arrives.
5. **The meaning of `GK` is not defined.** The Walkthrough sends `830WGK` after the mast confirms. FieldUnit sends it while the signal stick is held. Codeline should say whether `GK` means "signal control held" or "signal displaying proceed".
6. **The indication vocabulary is too small for the engine.** `SignalIndication` has 9 values. The engine uses 17: it adds `MEDIUM_*`, `SLOW_*`, `APPROACH_SLOW`, `APPROACH_DIVERGING`, `APPROACH_RESTRICTING` and `CAB_SPEED`. The CPL tests rely on `SLOW_CLEAR`. Either the cluster grows, or the docs define a mapping.
7. **"Unproven at startup" has no representation in the type system.** The C++ binding says `state()` starts "unproven/restrictive", but `TurnoutState` has only `Normal`, `Reverse` and `InTransit`. Is an unproven turnout `InTransit`? `SENSOR` has no unproven value at all (the stand-in starts `ACTIVE`).
8. **Two layers detect travel failure.** The appliance reports `FAULT STALL`, and the engine has its own 5 s correspondence timeout. Keep both: the engine's timeout also covers a link that fails mid-motion. The docs should say so.
9. **Debounce appears twice.** `HOLDOFF` is below the seam, and `TrackCircuit::dropoutDelay` repeats it above. Remove it from the engine, or define it as a vital drop-away time distinct from optical gap-bridging.
10. **The harvest map puts `SignalAspectPolicy` above the seam.** [Roadmap §5](../Roadmap.md#5-harvest-map) lists it with the engine. Rulebook translation to heads belongs in `MastCore`. The engine should emit an indication only.

---

## 7. What in the Engine Is Already Right (Keep It)

- **The scan shape** of sample, then evaluate, then drive, with time injected through `tick(nowMs)`. It maps directly onto read `state()`, evaluate, issue one-way commands.
- **Correspondence is checked against the demand** (`selfInCorrespondence`: reported equals commanded, and is a locked end). After the engine stopped inventing `MOVING`, this gave correct closed-loop behavior with no further change: a lost or slow command can never produce KR.
- **`TrackCircuit::isClear()` requires `VACANT` and `GOOD`.** The quality field (`LOST_COMMS`, `DEFECT`) already has a place for substituted values.
- **`InterlockingEngine`'s route evaluation**: least permissive along a route, most permissive across routes, incomplete routes at `STOP` (FieldUnit ADR 0009), sectional release, and engine return. None of it needed changing.
- **The detector-lock, route-lock and time-lock bits on `Switch`**, the `HSR` stick and fleeting in `SignalControl`, and the crossover and dependent-derail pairing.
- **The atomic `ControlTransaction` safety gate** in `applyControlTransaction`.
- **Fixed storage and no allocation after setup.** The `MAX_*` sizes are still on the harvest map's "leave" list.

---

## 8. Risks

- **The timer origin is the scan time.** The TER starts at the scan that *observes* the confirmed `STOP`, not at the appliance's own timestamp. Over MQTT, transport delay adds to the time lock. This errs on the safe side, but the effect grows with latency.
- **A mast that never confirms stops the plant.** Holding locks with no proof is safe but needs a maintainer to clear it. The operating rules must say what a dispatcher does when this happens.
- **The office indications lag one scan** in mode C. The office and the codec tests must accept that indications follow confirmation.
- **Snapshot discipline.** In process, the spike reads the mast's `state()` live during the scan. Over MQTT, `onStateChanged` can arrive mid-scan. Each scan must work from one snapshot of every appliance, as the spike already does for switches and sensors.
- **The pre-existing defects in section 5 are vital.** Item 1 (opposing signals) and item 2 (approach locking) matter more than any boundary change.
- **The stand-ins are not the M1a cores.** Real cores, their event ordering (`/state` against `/event` against LWT), and retained-state replay on reconnect may show behavior the stand-ins do not.

---

## 9. What the Spike Did Not Verify

- Real `TurnoutCore`, `SensorCore` or `MastCore`, or the MQTT loopback. Only the stand-ins were used.
- Arduino builds of the example sketches, and the memory cost of binding appliances on an ESP32.
- `AutomaticSignal` and `ControlPointLine` (APB, `HOME`/`DISTANT`) under the boundary. Their tests pass in B and C, but they derive `D` from the *computed* indication, whereas the [Walkthrough](../Walkthrough.md#apb-tumble-down--signal-cancellation) derives `DISTANT` from the mast's `/state`. No new scenario covers this.
- Electric locks (`CONTROL`/`WLK` substitution), fleeting under the boundary, crossovers and dependent derails under appliance faults (covered only by the existing tests), and `uint32_t` clock rollover.
- That "approach release waits for proof of `STOP`" is the correct relay model. It follows ASR needing the signal proven at stop, but the spike did not check it against FieldUnit's `docs/GLOSSARY.md` or its AAR primer (unverified).
- Whether the opposing-signal protection is expected to come from somewhere other than the control table, such as traffic locking in `ControlPointLine` within a single plant (unverified). The probe shows that the control table alone does not provide it.

The spike code (stand-ins, engine edits, patched tests, new scenarios, run scripts and result files) is in the session scratch directory and was not ported into this repository.
