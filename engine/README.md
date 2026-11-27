# MSpice engine

The engine is the simulator core of MSpice: a C++20 library (`mspice_core`) for transient
analysis of circuits with modified nodal analysis (MNA), and the command line tool `mspice`
that runs SPICE netlists with it.

What the engine contains:

- Devices: resistor, capacitor, inductor, independent voltage and current sources (DC,
  PULSE, SIN, PWL), diode (with breakdown, junction and diffusion charge), NPN/PNP
  (Ebers-Moll transport model with junction and diffusion charges), level-1 NMOS/PMOS,
  voltage-controlled switch, behavioural voltage and current sources with exact gradients
  (dual numbers), a behavioural gate driver and a fitted LED-strand model.
- Integration: backward Euler, trapezoid, variable-step BDF2; fixed or adaptive steps with
  step doubling or a predictor error estimate; breakpoints at source edges.
- Linear solvers: dense LU with partial pivoting, a fixed-structure sparse LU
  (`SparsePlan`), and optionally SuiteSparse KLU (off by default, see below).
- SPICE input: netlist reader with includes, `.lib` sections, `.param` scopes and subcircuit
  flattening, an expression parser compatible with ngspice B sources, and the mapping of the
  flattened netlist onto the simulator.

Status: early. Interfaces and file formats will change before version 1.0.

## Building

Requirements: CMake 3.21 or newer, Ninja, a C++20 compiler (GCC 12 or newer, Clang 15 or
newer). The commands below are run from the repository root; the build directory `build/`
is not tracked.

### Linux

```sh
sudo apt-get install cmake ninja-build g++      # Debian/Ubuntu
cmake -S engine -B build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine
```

For Clang, set the compiler when configuring:
`CC=clang CXX=clang++ cmake -S engine -B build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release`.

### Windows with MSYS2 (UCRT64)

Install [MSYS2](https://www.msys2.org/), open the "MSYS2 UCRT64" shell and install the
toolchain:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake -S engine -B build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine
```

### Windows with WinLibs

[WinLibs](https://winlibs.com/) provides a standalone MinGW-w64 GCC (UCRT runtime) that
includes CMake and Ninja. Add its `mingw64/bin` folder to `PATH`, then in PowerShell or
`cmd`:

```sh
cmake -S engine -B build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine
```

MinGW builds link the C++ runtime statically, so `mspice.exe` does not depend on a
`libstdc++-6.dll` found elsewhere in `PATH`.

### Build options

| Option | Default | Meaning |
|---|---|---|
| `MSPICE_BUILD_TESTS` | `ON` | Build the test suite. |
| `MSPICE_BUILD_BENCHMARKS` | `ON` | Build `mspice_batch_bench` (batched LU timing, not a test). |
| `MSPICE_USE_KLU` | `OFF` | Use SuiteSparse KLU for sparse MNA. KLU is LGPL-2.1; it is only linked as a shared library, and configuration fails if only static libraries are found. |
| `MSPICE_NATIVE_ARCH` | `OFF` | Compile the batched LU kernel with `-march=native`. The binaries then only run on CPUs like the build machine. |

The built-in solvers are the default and are what the test suite and CI check.

## Running the tests

```sh
ctest --test-dir build/engine --output-on-failure
```

The tests cover the solvers, device models, integration methods, the expression parser,
netlist flattening and the command line tool. The command line tests run both in-process
(`cli_in_process`) and against the built `mspice` binary (`cli_binary_*`); the negative
cases check the exit code and the exact error text.

## Command line usage

```text
mspice run [options] <netlist>   simulate a SPICE netlist (.tran) and print CSV
mspice --version                 print the version
mspice --help                    print the help

Options of run:
  -o, --output <file>   write the CSV to <file> instead of standard output
  --fixed-step          fixed steps of the .tran maximum step instead of adaptive steps
```

Example netlist `rc.cir` (an RC low-pass with a time constant of 1 ms driven by a 1 V step).
As in every SPICE netlist, the first line is the title and is not read as an element.

```spice
* RC low-pass
V1 in 0 PULSE(0 1 0 1n 1n 1 2)
R1 in out 1k
C1 out 0 1u
.tran 10u 5m
.end
```

```sh
build/engine/mspice run rc.cir -o rc.csv
```

The CSV has one column for the time, one per node voltage (`v(<node>)`) and one per
voltage source and inductor current (`i(<name>)`, positive from the first node through the
element to the second, as in ngspice). Names are lower case.

```text
time,v(in),v(out),i(v1)
0,0,0,0
...
```

### Supported netlist subset

- Elements: `R`, `C`, `L`, `V`, `I` (DC, `PULSE`, `SIN`, `PWL`), `D`, `Q` (three nodes),
  `M` (level 1, bulk tied to source), `S`, `B` (`V=` or `I=`), `E` and `G` (gain or
  `value=`/`cur=` expression), subcircuits (`X`, `.subckt`, `params:`).
- Directives: `.tran tstep tstop [tstart [tmax]] [uic]`, `.options reltol= abstol=
  method=trap|gear|euler`, `.ic v(node)=value`, `.param`, `.model`, `.include`, `.lib`,
  `.end`.
- Without `uic` a DC operating point is computed first; with `uic` the transient starts
  from 0 V or from the `.ic` values.

Anything else (for example `K`, `J`, `T`, `.ac`, `.dc`, `i(...)` inside expressions, model
parameters the engine does not implement) is reported as an error with file, line and
element name; nothing is ignored silently. Exit codes: `0` success, `1` netlist or
simulation error, `2` invalid command line.

Example error:

```text
$ mspice run bad.cir
bad.cir:3: error: z1: element type not supported
```

## Layout

| Path | Content |
|---|---|
| `include/mspice/` | Public headers (`core`, `sim`, `io`, `firmware`) |
| `src/` | Library sources |
| `apps/cli/` | The `mspice` command line tool |
| `apps/batch_bench/` | Batched LU benchmark |
| `tests/` | Test programs, SPICE fixtures (`tests/spice`), CLI netlists (`tests/cli`) |
| `docs/ARCHITECTURE.md` | Architecture overview |

## Licence

See the repository [LICENSE](../LICENSE): Apache License 2.0 with the Commons Clause
condition. Every source file carries the SPDX identifier
`LicenseRef-Apache-2.0-with-Commons-Clause`.
