# Tasks

This is the open work list of MSpice. It refines the stages of the
[roadmap](ROADMAP.md) into tasks with measurable acceptance criteria.

- **IDs.** Each task has a neutral ID with a prefix for its area: `RST`
  (Rust migration), `ENG`
  (engine), `LIB` (part library), `INT` (integration: KiCad, PCB, assembly),
  `SIL` (firmware co-simulation), `MCU` (microcontroller peripheral models).
  IDs are never reused; a dropped task keeps its ID with status `dropped`.
- **Stage.** Each task belongs to one roadmap stage. A stage is done when all
  of its tasks are `done`.
- **Origin.** The column "origin" names the label the task had in the earlier
  plan of the simulator this project grew out of (for example `V34`, `L1e`).
  It is only kept so that older commit messages can be traced.
- **Status.** One of `open`, `in progress`, `done`, `deferred`, `dropped`,
  `moved` (tracked in another repository, see "Repository split").
  Change the status in the overview table in the same pull request that
  changes the work. A task is `done` only when every acceptance criterion
  passes in CI and the tests named under "Tests" exist.
- **Issues.** Each section is written so that it can be copied into a GitHub
  issue unchanged. When an issue exists, add its number after the title in
  the section heading, for example `ENG-01 Newton robustness (#12)`.
- **Tests.** Every task needs positive tests (the feature works and gives
  the right numbers) and negative tests (wrong input or an unsupported case
  gives a clear error, nothing is dropped silently). Where results must stay
  "bit-identical", the existing test suite is run before and after and the
  outputs are compared exactly.
- **Thresholds.** Numeric limits marked "default" are settings, not
  constants; the proposed value can be changed in review.

Already in the engine and therefore not listed: SPICE reading with includes,
`.lib`, `.param` and subcircuit flattening; mapping of `R`, `C`, `L`, `V`,
`I`, `D`, `Q` (three nodes), `M` (level 1), `S`, `B`, `E`, `G`, `X`;
`.tran` with and without `uic`, `.ic`, `.options reltol abstol method`;
`mspice run` with CSV output; a report of nodes without a DC path to ground
before the run; junction and diffusion charges of diodes and BJTs; the
SPICE-like Newton profile (iteration limit, step cut, predictor) in
`mspice run`.

## Priority: Rust migration first

The engine and the library validator are being rewritten in Rust before any
other task is started (decision of the maintainers, 2026-11-27). The phases are
described in [RUST_MIGRATION.md](RUST_MIGRATION.md) and tracked here as
`RST-01` to `RST-05`. All tasks below are implemented in Rust once `RST-04` is
done; until then the C++ code only receives fixes that are needed to produce
the reference data of `RST-03`.

## Repository split

Tasks are implemented in the repository that owns the code. The engine side
of every feature stays in MSpice; user interfaces and GPL-dependent parts live
elsewhere.

| Repository | Tasks | Earlier labels |
|---|---|---|
| MSpice | RST-01..05, LIB-01..08, ENG-01..20, INT-01..09 (computation and file reading), SIL-00, SIL-01 (engine side) | V13, V15, V16, V17b, V18, V19, V20, V20a, V21, V24, V26, V27-3/4, V28a/b, V29, V30a, V30b (computation), V31, V33, V34, V35, V36, V37, V38, V39, V40, L1e/f, P2 |
| mspice-emu | EMU-01..04 (formerly MCU-01..04), emulator side of SIL-01, stages E0-E6 | V22 (emulator side), tickets 004-007 |
| mspice-studio | Display and editing of the results above: limit violations and traffic light (V30b), parasitics switch (V28, V40), progress and sweeps (V23), test runner (INT-09) | - |
| KiCad fork / mspice-kicad | INT-10, INT-11, INT-12; in KiCad: choosing part limits as symbol fields (V30a), parasitics switch per net (V28), dock panel, AI chat | - |

`mspice-kicad` (the separate process with API and MCP server, see
[KICAD_FORK.md](integration/KICAD_FORK.md)) gets its own repository under the
MSpice licence once INT-10 starts; until then INT-10 is tracked here.

## Overview

| ID | Title | Stage | Status | Depends on | Origin |
|---|---|---|---|---|---|
| RST-01 | Cargo workspace, CI, licence check of dependencies (phase R0) | 0 | open | - | Rust plan |
| RST-02 | Library validator in Rust, byte-identical output (phase R1) | 0 | open | RST-01 | Rust plan |
| RST-03 | Golden reference data from the C++ engine (phase R2) | 0 | open | RST-01 | Rust plan |
| RST-04 | Engine, SPICE import and CLI in Rust, same results and speed (phase R3), C++ removed (phase R4) | 0 | open | RST-02, RST-03 | Rust plan |
| RST-05 | Continue with the tasks below in Rust (phase R5) | 0 | open | RST-04 | Rust plan |
| LIB-01 | Engine instantiates library parts and runs their reference tests | 2 | open | - | roadmap |
| LIB-02 | Loads, state machines and noise sources in the engine | 2 | open | LIB-01 | roadmap |
| LIB-03 | Pin-pair limits and derating in the part format | 1 | open | - | V30a |
| LIB-04 | Connector and cable part kinds | 1 | open | - | V38, V39 |
| LIB-05 | Generic loads: lamp, pump, hub/PMSM/induction motors, solenoid, heater | 2 | open | LIB-01, LIB-02 | catalogue plan |
| LIB-06 | Generic sources and storage: batteries by chemistry, supercapacitor, solar panel, wind turbine, hub generator, mains, lab supply | 2 | open | LIB-01, LIB-02 | catalogue plan |
| LIB-07 | Interference sources: surge, ESD, fast transients; external magnetic field sources | 2 | open | LIB-02 (fields: INT-04) | catalogue plan |
| LIB-08 | Stimulus modes for driving quantities and shipped profiles | 2 | open | LIB-01 | catalogue plan |
| ENG-01 | Newton robustness: pn-junction limiting and gmin to ground | 3 | open | - | V34 |
| ENG-02 | Operating-point start with switchable start modes | 3 | open | ENG-01 | V31 |
| ENG-03 | Step-count limit and error messages | 3 | open | - | V36 |
| ENG-04 | Fast solver profile as library default | 3 | open | ENG-01, ENG-02 | 2.2 |
| ENG-05 | Small-signal AC analysis `.ac` | 3 | open | ENG-02 | V35 |
| ENG-06 | DC sweep `.dc` | 3 | open | ENG-02 | L1 |
| ENG-07 | SPICE import: BJT with Early effect, VDMOS, JFET | 3 | open | - | L1e, P2 |
| ENG-08 | SPICE import: `K`, `F`, `H`, `T`, current probes in expressions | 3 | open | - | L1, L1f |
| ENG-09 | Event-driven logic gates | 3 | open | - | V15 |
| ENG-10 | Event location in behavioural sources | 3 | open | - | V29, V33 |
| ENG-11 | Stepwise engine (`advance_to`, snapshot, restore) | 3 | open | ENG-10 | F3 |
| ENG-12 | Steady-state detection | 3 | open | - | V20a |
| ENG-13 | Periodic steady state by shooting | 3 | open | ENG-11 | V20 |
| ENG-14 | Period cache | 3 | open | ENG-11, ENG-13 | V19 |
| ENG-15 | Voltage and current limits per part, violation counting | 3 | open | LIB-03 | V30a |
| ENG-16 | Peak watcher with error band and traffic light | 3 | open | ENG-11, ENG-15 | V30b |
| ENG-17 | Behavioural block models for system runs | 3 | open | ENG-09 | V27-3/4 |
| ENG-18 | Progress output, parallel sweeps, Monte Carlo, fault injection | 3 | open | - | roadmap, V23, V32 |
| ENG-19 | Reference circuits and benchmark table | 3 | open | ENG-07 | 2.3 |
| ENG-20 | Speed ideas that need a measurement first | 3 | deferred | ENG-11 | V13, V16, V17b, V18, V21, V24, V26 |
| INT-01 | Read-only KiCad project import | 4 | open | - | roadmap |
| INT-02 | Multi-board assembly | 4 | open | INT-01 | ticket 025, V37 |
| INT-03 | Trace parasitics R and L from the PCB, switchable | 4 | open | INT-01 | V28a |
| INT-04 | PEEC parasitics with mutual inductance and capacitance | 4 | open | INT-03, ENG-08 | V28b, ticket 024 |
| INT-05 | Cable model from geometry | 4 | open | LIB-04 | V38 |
| INT-06 | Mated connector model | 4 | open | INT-02, LIB-04, ENG-15 | V39 |
| INT-07 | Automatic parasitic detection, crosstalk, windowed computation | 4 | open | INT-03, ENG-11 | V40 |
| INT-08 | EMC near field from simulated currents | 4 | open | INT-03 | tickets 023, 024 |
| INT-09 | Project sidecar file: off-board devices, probes, tests; `mspice test` | 4 | open | LIB-01, INT-01 | KiCad fork plan |
| INT-10 | `mspice-kicad` process: KiCad IPC API client, JSON-RPC API, MCP server | 4 | open | INT-01, RST-04 | KiCad fork plan |
| INT-11 | Live activity stream and log (selection, moves, edits; polling, file diff, fork event hook) | 4 | open | INT-10 | KiCad fork plan |
| INT-12 | Git from the editor and the AI (semantic diff of KiCad files, commit, pull, push) | 4 | open | INT-10 | KiCad fork plan |
| SIL-00 | Open co-simulation protocol (specification and conformance tests) | 7 | open | - | mspice-emu plan |
| SIL-01 | Emulator coupling through events and checkpoints | 7 | open | SIL-00, ENG-09, ENG-11 | V22 |
| MCU-01 | RMT peripheral model | 8 | moved | SIL-01 | ticket 004 |
| MCU-02 | MCPWM peripheral model | 8 | moved | SIL-01 | ticket 005 |
| MCU-03 | PCNT peripheral model with fan tachometer load | 8 | moved | SIL-01, LIB-02 | ticket 006 |
| MCU-04 | SAR ADC peripheral model | 8 | moved | SIL-01 | ticket 007 |

---

## Stage 1 and 2: library

### LIB-01 Engine instantiates library parts and runs their reference tests

- **Goal.** Parts in `library/parts` are only useful when the engine can
  simulate them. The spec (section 16) describes the mapping; the engine does
  not implement it yet.
- **Scope.** Read a part file, apply parameters and features, expand the
  model into engine elements (primitives, subcircuits, behavioural outputs,
  signals). Turn each `[[tests]]` entry into a small circuit, run it and
  compare the probes with the expected values and tolerances.
- **Acceptance.** Every generic part in `library/parts` passes its own
  reference tests in CI on Windows and Linux. A part whose test fails makes
  CI fail with the part name, test name, probe and deviation.
- **Tests.** Positive: each generic part, plus a part with a disabled
  feature (`when_disabled = "short"`). Negative: unknown model element,
  unknown feature, parameter out of range, probe on a missing node; each
  gives an error with file and key, nothing is skipped silently.
- **Depends on.** Library validator (stage 1).

### LIB-02 Loads, state machines and noise sources in the engine

- **Goal.** Roadmap stage 2: consumers with fixed value, CSV curve, formula
  or state machine, and noise sources, simulated against reference curves.
- **Scope.** Load branches as controlled current sources (resistance,
  power with `v_min`, curve lookup), state machines as event processes with
  threshold crossings and timeouts as breakpoints, noise sources as
  pre-generated time series from a seed.
- **Acceptance.** Each example load in `library/parts/loads` simulates
  against its reference curve within the tolerance in its part file. A state
  machine edge lands on the threshold crossing within 1 ns, independent of
  the maximum step. The same seed gives the same noise series bit for bit.
- **Tests.** Positive: each example load; a state machine with a timeout; a
  power load below `v_min`. Negative: CSV curve with non-monotonic x, state
  machine with an unreachable state, noise source without seed when one is
  required.
- **Depends on.** LIB-01.

### LIB-03 Pin-pair limits and derating in the part format

- **Goal.** Today a part can declare absolute limits per pin against one
  reference pin. Many datasheet limits are between two pins (drain-source,
  gate-source, reverse voltage, capacitor rating, reverse polarity of an
  electrolytic capacitor). ENG-15 needs them.
- **Scope.** A limit entry with a name, two pins, minimum and/or maximum
  voltage or current, and a source (datasheet reference or `assumption`).
  Optional derating threshold per part. Format version bump with migration
  note.
- **Acceptance.** Every rule has a positive and a negative test and a
  diagnostic code with `explain` text. The generic MOSFET, diode, BJT and
  capacitor parts carry pin-pair limits.
- **Tests.** Positive: MOSFET with `vds` and `vgs` (symmetric `+-`), capacitor
  with `vmin` for reverse polarity. Negative: limit on an unknown pin, same
  pin twice, minimum above maximum, missing unit.
- **Depends on.** -

### LIB-04 Connector and cable part kinds

- **Goal.** INT-05 and INT-06 need connector and cable data with sources
  instead of values typed into each project.
- **Scope.** Connector part: pitch, rows, pin length, contact resistance,
  inductance per contact, mutual inductance and capacitance to neighbour
  pins, rated current per contact, rated voltage, footprint name patterns
  for automatic matching. Cable part: conductor cross section (mm2 or AWG),
  conductor count, construction (pair, single with return distance,
  coaxial), insulation permittivity, optional maker values (Ohm/km, uH/m).
  Every value carries a source or `assumption`.
- **Acceptance.** Validator rules with positive and negative tests; at least
  two generic connectors (pin header, crimp housing) and two generic cables
  (twisted pair, coaxial).
- **Tests.** Negative: footprint pattern that matches two connector parts
  with equal priority, negative inductance, coaxial cable with inner
  diameter >= outer diameter.
- **Depends on.** -

---

### LIB-05 to LIB-08 Catalogue extension

- **Goal.** The generic loads, sources, storage devices and interference
  sources listed in [library/docs/CATALOG_PLAN.md](../library/docs/CATALOG_PLAN.md),
  each usable any number of times per project with adjustable parameters.
- **Scope.** One part file per entry, with stimulus modes (LIB-08:
  constant, steps, ramp, curve, sine, profile, random) for driving
  quantities and switchable features. Field sources (LIB-07) need INT-04 for
  coupling into board loops.
- **Acceptance.** As listed under "Acceptance (per part)" in the catalogue
  plan; every part has at least one engine-simulated reference test per
  supported stimulus mode.
- **Tests.** Negative: out-of-range parameter, missing profile file, feature
  dependency not met, stimulus mode not supported by the quantity.
- **Depends on.** LIB-01, LIB-02.

---

## Stage 3: engine

### ENG-01 Newton robustness: pn-junction limiting and gmin to ground

- **Goal.** With the SPICE-like profile (at most 10 Newton iterations, step
  cut 8) an astable multivibrator started with `uic` fails at t = 0: 5 V
  appears at once across a Schottky diode in front of an uncharged
  electrolytic capacitor, the exponential overshoots and 10 iterations are
  not enough. The engine limits the node-voltage change per iteration, but
  has no junction-voltage limiting as SPICE `pnjlim`.
- **Scope.**
  1. Junction-voltage limiting for diode, BJT (BE and BC) and MOSFET body
     diode as SPICE `pnjlim`, with `vcrit = n*Vt*ln(n*Vt/(sqrt(2)*Is))`,
     in the transient Newton and in the operating point. Switchable in
     `Settings`.
  2. Optional `Settings::gmin_ground` (default 0 = today's behaviour), set by
     the import from `.options gmin` / `rshunt`.
  3. A singular matrix during the run names the unknown (node or branch)
     instead of the generic "MNA singular or ill-conditioned matrix". This
     needs a typed exception from the linear solver through `Circuit::run`.
     The check before the run already names floating nodes.
- **Acceptance.** The astable multivibrator with `uic` runs in the SPICE-like
  profile and gives the same half period as the default profile within
  0.1 %. Existing tests unchanged; if a result is not bit-identical, the
  deviation is documented per test. A loop of voltage sources gives an
  error that names the sources.
- **Tests.** Positive: diode switched hard on from 0 V; BJT saturating from
  `uic`; `.options gmin=1e-9` changes the result of a node that only sees a
  capacitor. Negative: loop of voltage sources; negative `gmin`.
- **Depends on.** -

### ENG-02 Operating-point start with switchable start modes

- **Goal.** Like LTspice, a transient should start from a DC operating point
  by default. Two other start modes are needed for inrush and start-up
  checks. Today a failed operating point only gives a warning and the run
  silently starts from 0 V.
- **Scope.** Setting `start = operating_point | zero | ramp`:
  - `operating_point` (default): DC solution, then transient; `.ic` values
    act as constraints of the operating point.
  - `zero` (as `uic`): start from `.ic` or 0 V; shows inrush current.
  - `ramp` (as LTspice `startup`): sources ramp from 0 over an adjustable
    time, default 20 us.
  The operating point tries, in order, gmin stepping, source stepping and
  pseudo-transient. If all fail, the run stops with a clear error that names
  the step that failed; it never falls back silently. The import maps
  `.tran ... uic` to `zero` and `startup` to `ramp`. The run report names the
  start mode. A warning is given when a net marked as oscillator does not
  oscillate.
- **Acceptance.**
  - RC low-pass at 5 V: `operating_point` gives I(C) = 0 from t = 0; `zero`
    gives an inrush of 5 V/R; `ramp` gives a current peak of about
    C * 5 V / 20 us.
  - An astable multivibrator oscillates in all three modes (with `.ic`).
  - An op-amp band-pass with a behavioural op-amp (gain 1e5, output clamped
    with `min`/`max`) finds its operating point: bias 4.5 V, all op-amp
    outputs 4.5 V.
  - A symmetric multivibrator without `.ic` triggers the "does not
    oscillate" warning.
- **Tests.** Negative: a circuit without DC solution (two ideal voltage
  sources in parallel with different values) stops with an error naming the
  failed homotopy steps; `ramp` with a negative time is rejected.
- **Depends on.** ENG-01 (shares the limiting and gmin building blocks).

### ENG-03 Step-count limit and error messages

- **Goal.** `Circuit::run` stops after a fixed 3 000 000 steps. An 80 kHz
  sine over 0.3 s reaches that. Abort messages give time and step size but
  neither node nor element.
- **Scope.** `Settings::max_steps` (default as today, 0 = no limit), settable
  from the netlist through an option. Newton failures name the unknowns with
  the largest residual.
- **Acceptance.** A run with a small limit stops with the limit value in the
  message; the same run with 0 completes; existing results bit-identical.
- **Tests.** Positive: long sine run without limit. Negative: negative limit
  rejected; Newton failure message contains a node name.
- **Depends on.** -

### ENG-04 Fast solver profile as library default

- **Goal.** `mspice run` uses the SPICE-like profile (predictor error
  estimate, Newton limit 10, step cut 8, extrapolated start). The library
  `Settings` default still uses step doubling and up to 200 Newton
  iterations, so programs that use the C++ API directly are slower.
- **Scope.** Make the fast profile the default of `Settings`; keep the old
  profile selectable by name.
- **Acceptance.** All tests green, all reference circuits of ENG-19 pass in
  the default profile, including the astable multivibrator with `uic` and
  the op-amp band-pass. Runtime of each reference circuit recorded before
  and after.
- **Tests.** Positive: a test per profile name. Negative: unknown profile
  name rejected.
- **Depends on.** ENG-01, ENG-02. Switching earlier would break circuits
  that run today.

### ENG-05 Small-signal AC analysis `.ac`

- **Goal.** Frequency responses can today only be obtained with one sine
  transient per frequency (for a band-pass more than a hundred runs instead
  of one analysis). The import rejects `.ac`.
- **Scope.** Operating point (ENG-02), then complex MNA
  `(G + jwC) x = b` with the derivatives of the nonlinear elements at the
  operating point; behavioural sources supply them through dual numbers.
  Sources with `AC <magnitude> [phase]`. Sweeps `dec`, `oct`, `lin`. Output
  magnitude and phase per node. A shared `Circuit::linearize()` that ENG-20
  can reuse.
- **Acceptance.** RC low-pass against the analytic response; the op-amp
  band-pass in three board variants against an independent linear
  calculation within 0.1 % from 10 Hz to 100 kHz; runtime recorded.
- **Tests.** Negative: `.ac` with zero points; `AC` on a non-source element;
  operating point not found gives the ENG-02 error, not a result.
- **Depends on.** ENG-02.

### ENG-06 DC sweep `.dc`

- **Goal.** Transfer curves (diode, transistor, regulator line regulation)
  without transient runs. The import rejects `.dc`.
- **Scope.** Sweep of one or two source values or a `.param`, each point
  solved as operating point with the previous point as start value; output
  as CSV.
- **Acceptance.** Diode I-V curve against the Shockley equation within
  0.1 %; nested sweep of a BJT output characteristic against ngspice within
  0.5 %.
- **Tests.** Negative: sweep of an unknown source; step with the wrong sign;
  a point without solution names the sweep value.
- **Depends on.** ENG-02.

### ENG-07 SPICE import: BJT with Early effect, VDMOS, JFET

- **Goal.** BJT models without Early voltage (`VAF`) cannot be read; the
  engine has no Early effect at all. VDMOS models are read but not mapped;
  `M` only supports level 1 with bulk tied to source. `J` is missing.
- **Scope.** BJT: `VAF`, `VAR`, optional substrate node, `IKF`, `RB`, `RC`,
  `RE`. VDMOS subset (threshold, transconductance, body diode, gate
  capacitances). JFET level 1. Unsupported model parameters keep being
  listed as errors.
- **Acceptance.** An astable multivibrator with two small-signal NPNs runs
  through the import without any hand-written transistor code: half period
  and LED currents against ngspice within 1 %. Output characteristic of a
  BJT with `VAF` against ngspice within 0.5 %. A VDMOS switching stage
  against ngspice: switching times within 2 %.
- **Tests.** Negative: unknown BJT parameter, JFET with negative `beta`,
  VDMOS without required parameter.
- **Depends on.** -

### ENG-08 SPICE import: `K`, `F`, `H`, `T`, current probes in expressions

- **Goal.** Coupled inductors are needed for transformers and for INT-04 and
  INT-06; current-controlled sources and `i(...)` in expressions are common
  in vendor-neutral behavioural models; `T` covers simple lines.
- **Scope.** `K` with coupling factor between two or more inductors;
  `F` and `H` with a controlling voltage source; `i(Vx)` inside `B`, `E`
  and `G` expressions with exact derivatives; `T` lossless line.
- **Acceptance.** Ideal transformer with k = 0.999 against the analytic
  voltage ratio within 0.1 %; current mirror built from `F` against hand
  calculation; lossless line step response delay within one time step of
  `TD`; all compared with ngspice.
- **Tests.** Negative: `K` with |k| > 1; `K` referencing a missing inductor;
  `F` controlled by an element that is not a voltage source; `i()` of an
  unknown name.
- **Depends on.** -

### ENG-09 Event-driven logic gates

- **Goal.** Fast logic outputs force tiny time steps on the whole circuit
  when gates are modelled as analog behavioural sources. An event model
  removes that cost and gives SIL-01 its analog-to-digital boundary.
- **Scope.**
  - Primitive `logic_gate` generalised from the gate driver: input
    thresholds `k*Vcc + c` with hysteresis, truth table (`INV`, `BUF`,
    `NAND2`, `AND2`, ...), delay `tau(Vcc) = t0 * (Vcc/3.3)^n`.
  - Output as digital-to-analog boundary: Thevenin source with
    `R_out(Vcc)` and a linear ramp over `t_rise`.
  - Input crossings interpolated, step shortened to the crossing, output
    event at `crossing + tau`.
  - `threshold_sensor(net, vil, vih)` as analog-to-digital boundary.
  - A per-net mode `analog | digital` selects the model.
- **Acceptance.** A relaxation oscillator built from logic gates, once with
  analog gate models and once with the event model: frequency and duty
  cycle within 0.5 %, other node voltages within the profile tolerance,
  runtime of both recorded. The report notes that supply current spikes of
  the gates are not modelled in event mode.
- **Tests.** Positive: each truth table; hysteresis (input noise around the
  threshold gives one edge). Negative: gate with an input that is not
  connected; unknown gate type.
- **Depends on.** -

### ENG-10 Event location in behavioural sources

- **Goal.** Hard thresholds inside behavioural sources (`u()`, comparisons,
  very steep `tanh`) are not located in time; the edge lands wherever the
  time steps happen to fall. Large voltage deviations at switching nodes
  are then pure time shifts of steep edges, and results react
  non-monotonically to tolerance changes. Switches and the gate driver
  already switch at the interpolated crossing.
- **Scope.** Behavioural sources optionally report a distance to their
  threshold. When its sign changes within a step, interpolate the crossing,
  shorten the step and restart as after a switch event. Afterwards
  re-evaluate whether `trtol = 2` (LTspice default) and the charge
  tolerance on all nodes can become defaults.
- **Acceptance.** On a switching-regulator test circuit with a behavioural
  comparator, the edge position differs by less than 1 ns between
  `trtol` 1, 2 and 7. Existing results unchanged where no behavioural
  threshold exists.
- **Tests.** Positive: comparator driven by a slow ramp; edge time against
  analytic crossing. Negative: a source whose distance function is not
  continuous is reported, not silently treated as smooth.
- **Depends on.** -

### ENG-11 Stepwise engine (`advance_to`, snapshot, restore)

- **Goal.** Basis for shooting, the period cache, windowed parasitic
  computation and emulator coupling.
- **Scope.** Split `Circuit::run` into a `Transient` class holding state,
  linear solver, step size and result, with `start(Settings)`,
  `advance_to(t)`, `snapshot()`, `restore(snapshot)`,
  `set_level(net, level, t)` for digital inputs and `result()`.
  `Circuit::run` becomes a wrapper.
- **Acceptance.** All tests green; results bit-identical (pure
  restructuring); `advance_to` in 7 pieces gives the same result as one run;
  `restore` followed by the same `advance_to` reproduces the run.
- **Tests.** Negative: `advance_to` to a time before the current time;
  `restore` of a snapshot from another circuit.
- **Depends on.** ENG-10 (both change `Circuit::run`).

### ENG-12 Steady-state detection

- **Goal.** Like LTspice `.tran ... steady`: stop or report when a periodic
  circuit has settled.
- **Scope.** At each edge of a clock net, compare the period with the
  previous one (`sstol`-like tolerance); stop or report on settling.
- **Acceptance.** A switching regulator test circuit and an astable
  multivibrator report the settled state; the result up to that point is
  bit-identical to a run without detection.
- **Tests.** Negative: a circuit without a clock net gives a clear message
  that detection is not possible; a net that never settles is reported as
  such at the end.
- **Depends on.** -

### ENG-13 Periodic steady state by shooting

- **Goal.** Reach the periodic steady state without simulating the whole
  start-up.
- **Scope.** Newton on `x - Phi(x) = 0` with `Phi` the period map, Jacobian
  from differences or Broyden updates; `Phi` is kept for ENG-14.
- **Acceptance.** For a switching regulator test circuit the shooting result
  matches a long transient within the profile tolerance, with runtime
  recorded.
- **Tests.** Negative: non-convergence after the iteration limit is
  reported with the residual.
- **Depends on.** ENG-11.

### ENG-14 Period cache

- **Goal.** Long periodic runs (seconds of a PWM circuit) repeat nearly the
  same period many times.
- **Scope.**
  1. Hit-rate study first: log the state at each rising edge of a clock net,
     quantise with `q * (abs_tol + rel_tol * |x_i|)`, q in {1, 3, 10}, count
     hits with and without linear correction. Below 30 % hits in the settled
     part the cache is deferred.
  2. Cache keyed by the event sequence and the fast states; entries hold end
     state, `Phi`, events and peaks. Linear correction
     `x_end ~ x_end_cache + Phi (x_start - x_start_cache)` for slowly
     drifting states. Free-running oscillators use their own edges as
     window bounds; the phase error is reported.
  3. A guard recomputes a share of the hits; on deviation the entry is
     dropped. Report `cache: {hits, misses, recomputed, dropped}`.
- **Acceptance.** 1 s of a PWM test circuit and 30 s of an astable
  multivibrator, each against a reference run at sample points within the
  profile tolerance; runtime recorded.
- **Tests.** Negative: forced deviation in a cache entry is detected by the
  guard and the entry is dropped.
- **Depends on.** ENG-11, ENG-13.

### ENG-15 Voltage and current limits per part, violation counting

- **Goal.** For each part, say whether limits are violated, how often, how
  strongly and for how long, without storing whole waveforms.
- **Scope.**
  - Limit sources, in order of precedence: instance field in the
    schematic or sidecar > model file > library part (LIB-03). The report
    names the source of each limit. Parts without any limit are listed as
    "not checked".
  - Limit name to pin pair mapping per part type; ambiguous pins are an
    error, not a guess.
  - Per limit: number of violations (an event starts above the limit and
    ends below limit minus hysteresis, default 1 % of the limit), highest
    peak in V and in % over the limit with time, total and longest
    duration, first time; utilisation in % of the limit with a warning
    threshold (default 80 %); optional energy above the limit.
  - The report states the largest time step inside each event window; a
    peak closer than 5 % to the limit gets a hint "recompute finer".
  - Findings `abs_max` (error) and `derating` (warning) in the JSON report.
- **Acceptance.**
  1. Synthetic waveform with three separate peaks above the limit and noise
     around the limit: exactly 3 events; peak and duration against hand
     calculation.
  2. RLC circuit with analytically known overshoot: peak within 0.5 % of the
     analytic value.
  3. A capacitor without any limit appears under "not checked".
  4. A MOSFET switching stage with `vds` limit reports the same findings
     whether the limit comes from the instance, the model file or the
     library.
  5. Precedence instance > model file > library, one test each.
- **Tests.** Negative: limit on a pin pair the part does not have; limit
  with unit mismatch.
- **Depends on.** LIB-03.

### ENG-16 Peak watcher with error band and traffic light

- **Goal.** Judge how certain a limit finding is.
- **Scope.** Per pin maximum, minimum, time above limit, dV/dt, energy;
  works with the period cache. Recommended operating range and
  over/undervoltage thresholds with filter times (a 50 ns spike does not
  trip a protection with a 10 us filter). Error band from three sources:
  numerics (recompute a window around each critical peak with
  `rel_tol = 1e-5` using `restore`), part tolerances (corners or Monte Carlo,
  ENG-18), layout parasitics (INT-03; until then marked "not included").
  Result: green = margin larger than the band, yellow = smaller, red =
  violated.
- **Acceptance.** One test case each with deliberate overvoltage (red),
  marginal peak (yellow) and safe case (green); a protection threshold with
  filter time is not tripped by a short spike.
- **Tests.** Negative: filter time of zero is treated as "no filter" and
  reported; missing tolerance data is reported as "not included".
- **Depends on.** ENG-11, ENG-15.

### ENG-17 Behavioural block models for system runs

- **Goal.** System runs over long times need averaged or behavioural
  replacements for detailed switching stages.
- **Scope.** Behavioural blocks (averaged converter, regulator with control
  loop, LED string, logic block) with a test program that compares each
  block with its detailed counterpart over defined operating points.
- **Acceptance.** A system run with behavioural blocks is at least 20 times
  faster than the detailed run, and the compared quantities (output
  voltages, average currents, timing of power-good signals) agree within the
  tolerance stated per block.
- **Tests.** Negative: a block used outside its validated operating range
  gives a warning with the range.
- **Depends on.** ENG-09.

### ENG-18 Progress output, parallel sweeps, Monte Carlo, fault injection

- **Goal.** Roadmap stage 3 items for daily use.
- **Scope.** `--progress=json` (simulated time, steps, rejected steps,
  Newton iterations, estimated remaining time); parameter sweeps and Monte
  Carlo over all cores (default worker count = logical cores, fixed number
  possible; report names workers and physical and logical cores); fault
  injection (open pin, short between two nets, value at tolerance corner).
- **Acceptance.** A sweep of 100 variants on N cores gives the same results
  as a sequential run, bit for bit; speed-up recorded. Progress lines are
  valid JSON and monotonic in time. A fault-injected short is visible in
  the netlist report.
- **Tests.** Negative: zero workers with an explicit count, fault on an
  unknown pin, Monte Carlo without seed when reproducibility is requested.
- **Depends on.** -

### ENG-19 Reference circuits and benchmark table

- **Goal.** Public reference cases with independent reference values,
  replacing private benchmark circuits.
- **Scope.** At least: an astable multivibrator on three plugged boards with
  LED outputs, an op-amp band-pass on three plugged boards, a multi-board LED
  driver with a buck converter, a logic-gate oscillator. Each with netlist,
  expected values and the simulator that produced them (ngspice reference,
  regenerated, never only hand-made). A benchmark table per release:
  runtime and deviation of MSpice, ngspice and, where available, LTspice.
- **Acceptance.** All cases run in CI; the benchmark table is generated by a
  script, not by hand.
- **Tests.** Negative: a changed expected value without a changed reference
  run is rejected by the script.
- **Depends on.** ENG-07.

### ENG-20 Speed ideas that need a measurement first

Deferred until a measurement shows a benefit. Each item first delivers the
measurement named here.

- Generated step code: only if factorisation and solve exceed 30 % of the
  iteration time after ENG-07 and ENG-09; first try compiler flags.
- Subcircuits with their own time rate (multirate): first an offline split
  with an upper bound of the gain from reference waveforms; continue only
  if > 3x.
- Exact propagation of linear subcircuits inside a run: after ENG-11.
- Piecewise-linear switching regulators: a switched model against a
  detailed reference.
- Reduced-order replacement models: `Circuit::linearize()` (ENG-05) plus
  model order reduction, Bode comparison against the full model.
- Parallel subcircuits: only from about 500 unknowns.
- Parallel-in-time integration: research, after ENG-14.

---

## Stage 4: integration, PCB and assembly

### INT-01 Read-only KiCad project import

- **Goal.** Roadmap stage 4: simulate a KiCad project without exporting a
  netlist by hand.
- **Scope.** Read schematic and netlist, map symbols to library parts
  through fields and a sidecar file `<project>.mspice.toml`, never write to
  KiCad files.
- **Acceptance.** Hash of every KiCad file unchanged after every test; a
  test project gives the same results as its hand-written netlist.
- **Tests.** Negative: symbol without model mapping is listed with
  reference; missing sidecar entry; schematic and PCB out of sync.
- **Depends on.** -

### INT-02 Multi-board assembly

- **Goal.** A product often consists of several boards connected by
  connectors. Each design may be fitted more than once.
- **Scope.** Assembly file (designs, instances, connections) with schema.
  Nets are joined only through connections, never by equal names; equal
  unconnected names give a warning. Joined net name stays plain when all
  parts have the same name. Reference mode `plain` (duplicate references
  are an error with a list) or `qualified` (`<instance>.<ref>`).
  `fitted: false` marks an instance as not fitted. Optional contact
  resistance per pin instead of joining. Connection type `bus`: several
  connectors on one bus, equal pin numbers form one net. Virtual designs:
  one KiCad project split by sheet paths. Export with models: diodes,
  transistors and op-amps are mapped through the library or a supplied
  model file instead of being left open.
- **Acceptance.** Three stacked boards with a shared 1x6 bus (astable
  multivibrator and op-amp band-pass of ENG-19) give the same results from
  the KiCad projects as from the hand-written netlists, for every plug
  variant via `fitted: false`.
- **Tests.** Negative: pin used in two pair connections, duplicate
  references in `plain` mode, connection to a missing pin, bus with
  mismatched pin counts.
- **Depends on.** INT-01.

### INT-03 Trace parasitics R and L from the PCB, switchable

- **Goal.** Today every trace is ideal. Overshoot at fast edges is
  underestimated. Trace resistance and inductance should come from the
  routed layout automatically, without values typed by hand.
- **Scope.**
  - Switch `parasitics = off | auto | selected | full`, global and per net.
    `off` (default) gives today's netlist; `auto` uses INT-07; `selected`
    uses rules (gate drivers, switch nodes, power loops); `full` covers all
    nets including connectors between boards. The report names the mode
    and, per net, whether parasitics were included.
  - Read the PCB file: segments, arcs, vias, pads, stack-up with copper
    thickness and dielectric. Build the pad-to-pad tree per net.
  - Per segment R from length, width, thickness and temperature; L as
    partial self-inductance of a rectangular conductor, over a plane as
    microstrip loop inductance; vias with their own formula.
  - Merge short segments (for example < 0.1 nH or < 1 mOhm).
  - Split the schematic net at the pads; if schematic and layout do not
    match, report and keep the net ideal. Copper zones are marked ideal.
  - Before the run: table per net with pad-to-pad R and L, plus a
    subcircuit per net.
- **Acceptance.** Straight trace 50 mm x 0.5 mm over a plane against the
  microstrip formula within 5 %; via against formula; merged chain against
  unsplit chain at 1 MHz within 1 %; two traces against an external
  calculator. Astable multivibrator with `off` and `full`: same results
  within tolerance (null test, slow edges), runtime recorded. A fast
  half-bridge shows the expected `L * di/dt` spike. `off` gives a
  bit-identical netlist.
- **Tests.** Negative: PCB file without stack-up gives an error naming the
  missing data; pad number missing in schematic.
- **Depends on.** INT-01.

### INT-04 PEEC parasitics with mutual inductance and capacitance

- **Goal.** Above some 100 kHz current follows the lowest impedance
  `R + jwL`, not the lowest resistance; return current flows under the
  forward trace. Mutual inductance and plane capacitance matter for fast
  nets.
- **Scope.** Partial self and mutual inductances of rectangular segments
  (closed formulas after Grover/Ruehli), across layers; solve R + jwL per
  frequency; model order reduction to a few pad nodes; coupled inductors
  (`K`, ENG-08); later capacitance to the plane. Same output as INT-03.
  Abort before computing when the dense matrix would exceed 4 GB, naming
  the grid size needed; warning when the condition number exceeds 1e12.
- **Acceptance.** Self inductance of a straight rectangular conductor and
  mutual inductance of two parallel conductors within 2 % of the formulas;
  two return paths (short under the trace, long beside it): at 1 kHz split
  by R, at 10 MHz >= 90 % under the trace; limit f -> 0 matches the DC
  distribution within 1 %.
- **Tests.** Negative: matrix size limit; ill-conditioned case gives the
  warning; f = 0 handled as DC.
- **Depends on.** INT-03, ENG-08.

### INT-05 Cable model from geometry

- **Goal.** Cable L, R and C must today be calculated and entered by hand.
- **Scope.** From the cable part (LIB-04) and a length: R (with
  temperature), loop or self and mutual inductance (two-wire
  `L = mu0/pi * l * acosh(D/d)`, coaxial `mu0/(2 pi) * l * ln(D/d)`),
  capacitance and coupling factor. Number of sections chosen so a section is
  shorter than about 1/10 of the wavelength at the highest relevant
  frequency (from the edge rate), with an upper bound. Hand values remain
  possible and take precedence; the report names the source and formula.
  Usable for external loads and between boards (INT-02).
- **Acceptance.** Two-wire line and coaxial cable within 1 % of the
  analytic formulas; plausibility against a maker datasheet (Ohm/km, uH/m)
  with source.
- **Tests.** Negative: zero length, distance smaller than conductor
  diameter.
- **Depends on.** LIB-04.

### INT-06 Mated connector model

- **Goal.** Mated connections are ideal today, or have one contact
  resistance for all pins. Inductance, crosstalk, return path over ground
  pins and contact overload are missing.
- **Scope.** Connector type from the footprint (LIB-04), override per
  symbol field or per assembly connection; unknown type gives a message and
  a value from the pin length (marked as assumption). R and L per contact,
  second version with mutual inductance to neighbours and pin-to-pin
  capacitance. Loop inductance from the real pin assignment and the distance
  to ground pins; fast signals and power paths without a neighbouring ground
  pin are listed. Current sharing on parallel contacts; per contact current
  against rating, voltage against rating, I^2 R loss, with counting as in
  ENG-15 (finding `connector.overcurrent`). Variants: fitted, not fitted,
  one contact open, one contact with raised resistance. Optional hot
  plugging: contacts as timed switches with mating order and bounce.
  Connector - cable - connector as one connection.
- **Acceptance.**
  1. Contact L against datasheet or formula within 10 %.
  2. Two parallel contacts with unequal R: current sharing within 0.5 % of
     hand calculation; overload of one contact is reported although the
     total is below the sum of ratings.
  3. Loop inductance with 1 and with 3 neighbouring ground pins against
     formula: direction and order of magnitude correct.
  4. Astable multivibrator over the board connector with `off` and `full`:
     null test within tolerance.
  5. Hot plugging into an RC input through a cable: peak current within 2 %
     of the analytic RLC solution.
  6. `parasitics = off` gives today's netlist.
- **Tests.** Negative: connector type matched by two library entries;
  contact rating missing (listed as "not checked").
- **Depends on.** INT-02, LIB-04, ENG-15. Coupling together with INT-04.

### INT-07 Automatic parasitic detection, crosstalk, windowed computation

- **Goal.** The program decides where trace, connector and cable
  inductance matters, includes the effect of fast signals on other nets,
  and keeps runtime low by computing details only at switching edges.
- **Scope.**
  1. Detector (after INT-03): aggressor candidates from parts (MOSFET drain
     and gate, gate driver outputs, regulator switch nodes, PWM pins, logic
     outputs with known rise time, clocks, shunt and LED paths). A short
     pre-run without parasitics measures dV/dt per net and di/dt per branch.
     A net is included when `L * di/dt` > max(50 mV, 2 % of swing) or > 10 %
     of the margin to its ENG-15 limit; when the line length exceeds
     rise time x propagation speed / 6; or when the resonance of L with the
     net capacitance lies below the edge bandwidth (about 0.35/tr).
     Thresholds are settings. A table per net with decision, reason and
     numbers before the run; override per net (`always`, `never`). This is
     `parasitics = auto`.
  2. Crosstalk and shared impedance: victim candidates (high-impedance
     inputs, ADC pins, comparator and op-amp inputs, references, feedback
     dividers, gates, reset/enable, logic inputs); coupled pairs from the
     layout; capacitive `Cm * dV/dt * Z` and inductive `Lm * di/dt`
     estimates; only pairs above a threshold (for example 10 % of the
     victim's noise margin) enter the model. Shared ground or supply copper
     between a load current and a sensitive signal becomes a shared element
     (`R * i + L * di/dt`).
  3. Windowed computation (after ENG-11): main run with few parasitics; at
     each detected aggressor edge a window with the full model is
     recomputed from a snapshot, until the ringing decays (for example 5
     resonance periods, bounded). At the window end the states are compared;
     if they differ beyond tolerance, the report recommends continuous mode
     for that net. Identical edges are computed once or sampled (shared with
     ENG-14).
  Switches: `parasitics`, `crosstalk = off | auto | full`,
  `shared_impedance = off | auto`, `parasitic_mode = continuous | window`.
  Crosstalk and ground shift results feed ENG-15 (`xtalk.*`,
  `groundshift.*`).
- **Acceptance.**
  1. On a multi-board LED driver with a buck converter, the detector selects
     gate drivers, MOSFET drains and LED paths, and not dividers, pull-ups or
     DC logic supply.
  2. Astable multivibrator: the detector selects (almost) nothing.
  3. Two parallel traces of known geometry: crosstalk peak within 10 % of the
     analytic solution or of ngspice with coupled lines.
  4. Shared ground section with a load-current step: ground shift at the
     signal within 5 % of `R * i + L * di/dt`.
  5. Windowed against continuous computation on the same case: peaks within
     2 %, runtime recorded; the back-action check fires on a deliberately
     lossy case.
  6. All switches `off`: netlist and result bit-identical to today.
- **Tests.** Negative: override for an unknown net; window longer than the
  run.
- **Depends on.** INT-03, ENG-11 (window part).

### INT-08 EMC near field from simulated currents

- **Goal.** Show where magnetic near fields arise above a board, as with a
  near-field probe, from the currents of a finished simulation.
- **Scope.** Biot-Savart for finite straight segments (closed formula,
  quasi-static). Spectrum of the selected branch currents (uniform resampling,
  Hann window with amplitude correction, strongest lines up to an adjustable
  frequency, DC and neighbouring bins excluded). Current distribution on the
  copper either from a DC resistive solve (upper estimate) or from PEEC
  (INT-04). Field planes at adjustable heights above the board, per line and
  as envelope; output as array file, JSON summary (lines, maxima with
  position, assumptions) and an image.
- **Acceptance.** Field of a straight finite conductor against the analytic
  Biot-Savart result within 1 %; PEEC distribution with f -> 0 equals the DC
  distribution within 1 %; DC distribution mode stays bit-identical when PEEC
  is added.
- **Tests.** Negative: run without the requested current probes; height
  below the board surface; time window outside the run.
- **Depends on.** INT-03; INT-04 for the PEEC mode.

---

### INT-09 Project sidecar file and `mspice test`

- **Goal.** Off-board devices, probes, stimulus profiles and test definitions
  per project, without touching KiCad files.
- **Scope.** `<project>.mspice.toml` format (schema, validator codes),
  attaching devices to connector pins, `[[tests]]` with expectations,
  `mspice test` CLI with JSON output. See [KICAD_FORK.md](integration/KICAD_FORK.md)
  sections 4 and 8.
- **Acceptance.** Example project with a battery, a fan and an LED strip on
  connectors runs; tests report measured values; KiCad files unchanged (hash
  check before and after).
- **Tests.** Negative: unknown connector pin, device pin left open, test with
  an unknown probe, expectation without tolerance.
- **Depends on.** LIB-01, INT-01.

### INT-10 `mspice-kicad` process with API and MCP server

- **Goal.** One process that connects KiCad (IPC API), MSpice and AI clients.
- **Scope.** As in [KICAD_FORK.md](integration/KICAD_FORK.md) sections 3 and
  10: project model, netlist builder, JSON-RPC API, MCP server (stdio and
  HTTP) with read, simulate, render, check and edit tools; edits only through
  the KiCad IPC API.
- **Acceptance.** An MCP client can open a project, run a simulation, read a
  waveform, run DRC and get a board render; each tool has a test against a
  recorded KiCad IPC session.
- **Tests.** Negative: KiCad not running, unsupported KiCad API version,
  invalid tool arguments (exact error), edit rejected by KiCad.
- **Depends on.** INT-01, RST-04.

### INT-11 Live activity stream and log

- **Goal.** The AI and the user can follow what happens in the editors, live
  and afterwards. See [KICAD_FORK.md](integration/KICAD_FORK.md) section 9a.
- **Scope.** Event model (select, move, rotate, add, delete, edit, route, by
  user or AI), sources IPC polling, file watcher with semantic diff of KiCad
  files, optional fork event hook; JSON-lines log; watch mode for the chat
  with batching; privacy settings.
- **Acceptance.** Moving a footprint in the PCB editor appears in the stream
  within 300 ms (polling) and in the log with old and new position; a saved
  schematic change appears as a semantic diff entry; recording off means no
  file is written.
- **Tests.** Negative: KiCad closed during recording (clean stop, message),
  corrupt autosave file (skipped with warning), log folder not writable.
- **Depends on.** INT-10.

### INT-12 Git integration

- **Goal.** Version control without leaving the editor, usable by the AI with
  user confirmation. See [KICAD_FORK.md](integration/KICAD_FORK.md) section 9b.
- **Scope.** Status, semantic diff, commit (with save first), pull (with
  reload), push, log, branch, through the installed `git`; MCP tools.
- **Acceptance.** Commit, pull and push work against a test remote; a pull
  with a conflicting KiCad file reports the conflicting items and changes
  nothing; AI-initiated push waits for confirmation.
- **Tests.** Negative: unsaved documents on pull, no remote configured,
  authentication failure (message from git passed through), force-push
  request refused.
- **Depends on.** INT-10.

---

## Stage 7: firmware co-simulation

### SIL-00 Open co-simulation protocol

- **Goal.** One documented protocol between the engine and any firmware
  endpoint (emulators in the separate GPL repository mspice-emu, other
  emulators, hardware adapters), so that MSpice never links GPL code and
  third parties can add endpoints under any licence.
- **Scope.** Specification in `docs/protocol/`: transport (TCP on localhost,
  Unix socket, Windows named pipe), framing (length-prefixed CBOR, JSON lines
  for debugging), handshake with version and capabilities, pin map, time
  model (the engine grants a horizon with `advance`; the endpoint never goes
  back in time; the engine rolls back to its own snapshots as in SIL-01),
  messages `edge`, `pwm`, `pulses`, `sample`/`value`, `uart`, optional
  endpoint `checkpoint`/`restore`, `bye`. Conformance test suite usable by
  both sides; a scripted fake endpoint for engine tests. Draft message set:
  mspice-emu `docs/PLAN.md`.
- **Acceptance.** Specification reviewed by both repositories; the fake
  endpoint passes the conformance suite; the suite runs in CI.
- **Tests.** Positive: handshake, each message type round trip. Negative:
  wrong protocol version, unknown pin, time stamp before the granted
  horizon start, malformed or oversized frame, endpoint silent past a
  timeout (clear error, no hang).
- **Depends on.** - (can be designed during the Rust migration so that the
  engine API fits it).

### SIL-01 Emulator coupling through events and checkpoints

- **Goal.** Firmware runs of a second or more next to the analog circuit,
  with the emulator in a separate process over an open protocol.
- **Scope.** The emulator owns time and cannot go back. The engine computes
  speculatively up to a horizon and stores snapshots; when the emulator
  reports an edge in the engine's past, the engine restores the snapshot.
  ADC reads are mandatory synchronisation points. Line protocol with
  commands for run, input event, snapshot, restore and ADC read; a
  coordinator; first tested against a synthetic event source (recorded pin
  trace).
- **Acceptance.** With the synthetic source, the result is bit-identical to
  fixed 1 us time slices. A firmware toggling a pin and reading an ADC
  channel runs end to end; rollbacks are counted in the report.
- **Tests.** Negative: protocol line with unknown command; restore of an
  unknown snapshot; emulator process exits early (clear error, no hang).
- **Depends on.** SIL-00, ENG-09 (threshold sensor), ENG-11.

---

## Stage 8: microcontroller peripheral models

Moved to the [mspice-emu](https://github.com/H-Held/mspice-emu) repository
(`docs/PLAN.md`, tasks EMU-01 to EMU-04), because the peripheral models run on
the emulator side, which is GPL-licensed.
