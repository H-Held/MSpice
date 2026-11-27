# Engine architecture

This is an overview of the code in `engine/`. Details of each function are in the Doxygen
comments of the headers under `include/mspice/`.

## Data flow

```text
SPICE netlist (.cir)
   |  io::read_spice_file()        logical lines, includes, .lib sections, comments
   v
io::Deck
   |  io::flatten()                .param scopes, .model, subcircuit expansion,
   |                               expressions parsed and resolved, report of
   v                               unsupported constructs
io::FlatNetlist
   |  io::build_simulation()       elements -> Circuit calls, .tran/.options/.ic ->
   v                               Settings; every unsupported item is an error
sim::Circuit + sim::Settings
   |  Circuit::run()               MNA assembly, Newton, time-step control
   v
sim::Result                        samples of node voltages and probed currents
   |  apps/cli                     CSV on standard output or in a file
```

Programs can also build a `sim::Circuit` directly through its C++ API and skip the netlist
layer; most tests do this.

## Modules

| Module | Headers | Responsibility |
|---|---|---|
| core | `core/version.hpp` | Version string. |
| sim | `sim/circuit.hpp` | Device stamps, transient solver, result, netlist export as JSON (`describe_json`). |
| sim | `sim/linear_solver.hpp` | Factor and solve the MNA matrix: dense LU, `SparsePlan`, optional KLU. |
| sim | `sim/sparse_plan.hpp` | Fixed-structure sparse LU compiled to an operation list. |
| sim | `sim/batched_plan.hpp` | The same operation list applied to many matrices at once (structure-of-arrays, SIMD); used by the batch benchmark. |
| sim | `sim/linear_exact.hpp` | Near-exact propagator for linear systems `C x' + G x = B u(t)` with piecewise-linear inputs. |
| sim | `sim/dual.hpp` | Forward-mode dual numbers `Dual<N>` for exact Jacobians of behavioural sources. |
| io | `io/spice_netlist.hpp` | Netlist reading and flattening with ngspice naming. |
| io | `io/spice_expr.hpp` | Expression parser, constant folding, compilation to a flat tape, evaluation on `double`, `long double` and `Dual<N>`. |
| io | `io/spice_import.hpp` | Mapping of a flattened netlist onto `Circuit` and `Settings`. |
| firmware | `firmware/pin_bridge.hpp` | Digital pin events with jitter and Schmitt-trigger inputs; a building block for later co-simulation, not an emulator. |

## Transient solver

`Circuit::run()` assembles the MNA system (node potentials plus one branch current per
voltage source, inductor and behavioural voltage source) and integrates it in time.

- **Integration methods:** backward Euler, trapezoid (default) and variable-step BDF2. After
  source edges, breakpoints and switching events the solver restarts with a short backward
  Euler step.
- **Nonlinear devices** (diode, BJT, MOSFET, behavioural sources, LED strand) are linearised
  in a Newton iteration. Behavioural sources written as generic lambdas get exact gradients
  through `Dual<N>`; others fall back to finite differences. Junction and diffusion charges
  are integrated charge-conserving with the circuit's method. A GMIN of 1e-12 S is placed
  across nonlinear branches and the node-voltage change per Newton iteration is limited.
- **Step control:** fixed steps of `Settings::max_step`, or adaptive steps with a local
  truncation error estimate. `LteMethod::StepDoubling` (default) compares one full step with
  two half steps; `LteMethod::Predictor` compares the trapezoid result with a quadratic
  extrapolation and needs one solve per step. Only charge-storage unknowns (capacitor nodes,
  inductor currents) enter the estimate, as in SPICE.
- **SPICE-like Newton settings** (all opt-in, library defaults keep the historical results
  bit for bit): iteration limit per attempt (`newton_max`, SPICE ITL4), step cut after a
  failed attempt (`newton_cut`), a Newton start extrapolated from the last two points
  (`predict_guess`), the charge tolerance `lte_chgtol` (SPICE CHGTOL) and a truncation
  error factor `trtol`. `mspice run` enables the predictor estimate, `newton_max = 10`,
  `newton_cut = 8` and `predict_guess` for adaptive runs.
- **DC operating point:** with `Settings::operating_point` a DC solution at t = 0 is solved
  first (capacitors open, inductors shorted).
- **Events:** switches and the gate driver change state at the interpolated crossing time
  of their control voltage; the step is shortened to land on it.

## Linear solvers

`LinearSolver` hides the backend behind `factor()` and `solve()`. `Settings::linear_backend`
selects it:

- `Dense`: partial-pivot LU on the full matrix.
- `Plan`: `SparsePlan`. On the first matrix it chooses a pivot order, computes the fill-in
  and records the elimination as a flat list of multiply-subtract operations. Later matrices
  with the same pattern are factored by replaying that list without any search. If a pivot
  becomes too small relative to its build-time value, the plan is rebuilt or the dense LU
  takes over for that matrix.
- `Auto` / `Klu`: SuiteSparse KLU when the engine was built with `MSPICE_USE_KLU=ON` and the
  matrix has at least 8 unknowns, otherwise dense LU. KLU keeps its symbolic analysis while
  the stamped pattern is unchanged and refactors numerically; a refactor whose quality drops
  is repeated with fresh pivoting, and results are iteratively refined.

The MNA buffers are shared across all steps of a run. The stamped pattern is the union over
the run, so it becomes constant once every device has stamped, and only touched entries are
cleared per Newton iteration. With `Settings::reuse_matrix` the LU factors are reused when
the matrix is exactly identical to the previous one.

## Expressions

`io::parse_expression()` builds a tree; `resolve()` replaces parameters by constants and
renames nodes and devices to their flattened names; `fold()` evaluates constant subtrees;
`compile()` produces a `Tape` of postfix instructions with registers for repeated
subexpressions and a list of time breakpoints (from `u(time - c)`, `time > c`, ...).
`evaluate(tape, v)` is a template and runs on `double`, `long double` (reference values in
tests) and `Dual<N>` (exact gradients), so a tape plugs into the behavioural sources of
`Circuit` without hand-written derivatives.

## Errors and diagnostics

- The netlist reader never drops input: every construct it cannot handle is a report entry
  (`unsupported`, `approximation`, `ignored`, `note`) with file and line.
- `build_simulation()` turns every `unsupported` entry into an error and rejects elements,
  model parameters and directives it does not implement.
- The solver throws `std::runtime_error` for singular matrices or Newton failure and
  `std::invalid_argument` for invalid device parameters.

## Tests

Each test program in `tests/` is registered with CTest, often once per mode. Reference
values come from analytic solutions (RC, RLC, diode and BJT equations), from independent
re-implementations inside the test, or from values measured with ngspice and written into
the test. The CLI tests run both in-process and against the built binary.
