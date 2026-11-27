# Rust migration plan

Branch: `rust`. Goal: replace the C++ engine and the C++ library validator by
Rust without losing correctness or speed, then continue with
[TASKS.md](TASKS.md) in Rust.

## Why

- One build tool (`cargo`) on Windows, Linux and macOS; no CMake/MSYS2 setup.
- Memory safety for parsers that read untrusted netlists and part files.
- WebAssembly build for a browser demo (roadmap stage 10) with little effort.
- Easier outside contributions.

Speed is not expected to improve from the language itself; it must not get
worse.

## Target layout

```
Cargo.toml                 workspace
crates/
  mspice-core/             circuit, MNA, solvers, devices, transient engine
  mspice-spice/            SPICE netlist lexer, parser, expressions, import
  mspice-cli/              binary `mspice`
  mspice-libformat/        library data model, TOML loading, validation
  mspice-libcheck/         binary `mspice-libcheck`
library/                   data, docs, tests (unchanged location)
engine/                    C++ reference implementation, removed in phase R4
```

## Phases and acceptance

| Phase | Content | Acceptance |
|---|---|---|
| R0 Setup | Cargo workspace, CI (fmt, clippy -D warnings, test on Windows/Linux), `cargo-deny` with a licence allowlist (MIT, Apache-2.0, BSD-2/3, ISC, Zlib, Unicode-3.0, MPL-2.0 for unmodified crates only) | CI green, `cargo deny check` green |
| R1 libcheck | Port `mspice-libcheck` | All existing fixtures in `library/tests` give byte-identical text and JSON output to the C++ tool; `check --deny-warnings library/` clean; all 58 codes have a negative test |
| R2 Golden data | Before porting the engine, generate reference outputs with the C++ engine: every C++ test circuit plus a benchmark set, as CSV with step counts and Newton iteration counts | Golden files committed under `engine-golden/` with the generating command |
| R3 Engine | Port core, SPICE import and CLI; sparse LU in Rust (own or `faer`); KLU not used | All C++ tests ported (positive and negative, same messages); waveforms match golden data within 1e-9 relative or the tolerance the original test used; benchmark median time no worse than C++ (same machine, release, `-C target-cpu` default) |
| R4 Switch | Remove `engine/` and the C++ libcheck, update READMEs, CI and THIRD_PARTY.md | Only Rust builds in CI; docs describe the Rust build |
| R5 Tasks | Continue with TASKS.md in stage order | Per task as defined in TASKS.md |

If a phase cannot meet its acceptance, it stays open and is documented; the
C++ code remains the reference until R4.
