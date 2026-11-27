# MSpice design (2026-11-27)

Status: agreed in conversation with Henry Held and his project partner, 2026-11-27.
This document is the reference for everyone (people and agents) working on the
repository. The ordered work list is [ROADMAP.md](../ROADMAP.md).

## Intent

MSpice is a fast circuit simulator with an open, documented part library. It
started as the internal simulator `csim` of a KiCad co-simulation project at
HTW Berlin. A professor plans to build a KiCad-based editor with native AI
(MCP) integration on top of it; that editor lives in a separate repository.

MSpice itself must stand on its own:

- usable from a terminal on Windows and Linux, scriptable, with machine-readable
  output (JSON) so that AI agents and other programs can drive it;
- embeddable in other programs through a stable API (C API, later JSON-RPC
  over stdio);
- fast: speed is a design goal, not an afterthought;
- easy to extend with new parts, by people and by language models, from
  datasheets or from openly licensed SPICE models;
- safe to use and to publish: nothing in the repository may require a licence
  we do not have, and nothing may force us to take the project offline.

Companies should be able to use it at work and should want to publish models
for it.

## Goals

1. Engine (`engine/`, later `crates/`): simulator core and command line tool `mspice`.
2. Library (`library/`): an open part library format, a validator with good
   error messages, generic standard parts, configurable loads (consumers), and
   guides for creating parts.
3. Studio: a separate, modern GUI in its own repository `mspice-studio` (Rust).
4. Tests everywhere: every feature has positive tests (valid input, expected
   result) and negative tests (invalid input, exact expected error).
5. Documentation that a newcomer can follow without asking.

## Non-goals (for now)

- Shipping a KiCad fork. The KiCad-based editor is a separate repository
  (GPLv3, like KiCad itself). The bridge code of the old project stays internal.
- Bundling vendor SPICE models (TI, ADI/LTspice, Infineon, ...). Users import
  them locally; MSpice never redistributes them.
- Microcontroller emulation in the first stages (see roadmap stage 7/8).

## Anti-goals (would be a failure even if it works)

- Any file in the repository that we may not redistribute.
- Vendor or trademark names as part identifiers (`generic_*` names are used;
  real part numbers only appear in a `compatible` field).
- Silent behaviour: unsupported input is always reported, never dropped.
- Writing to a user's KiCad files (see "KiCad files" below).
- Documentation that reads like marketing.

## Licensing

| Area | Licence | Reason |
|---|---|---|
| Code (engine, tools, studio) | Apache-2.0 with Commons Clause | Use, modify and use commercially allowed; selling MSpice itself is not. Apache-2.0 brings the warranty and liability disclaimer and a patent grant. |
| Part models in `library/parts` | CC BY 4.0, per file `license` field | Companies keep credit and can contribute models without the sale restriction. |
| Contributions | DCO sign-off (`git commit -s`) | Contributor confirms the right to submit; no CLA paperwork. |

Consequences, recorded so nobody is surprised:

- Commons Clause makes the code "source available", not OSI open source.
- No GPL code may enter the repository. GPL tools (QEMU, simavr, ngspice for
  comparison) are only used as separate processes or as optional external
  references, never linked or copied.
- SuiteSparse KLU (LGPL-2.1) is optional (`MSPICE_USE_KLU`, default OFF) and
  only linked dynamically. The built-in sparse LU is the default.
- Before the first public release the licence is reviewed by a lawyer or the
  HTW legal office.

Research notes with sources: [licensing-research.md](licensing-research.md).

## Repository layout

```
MSpice/
  engine/     C++ core library, CLI `mspice`, tests, benchmarks
  library/    format spec, schema, validator, generic parts, loads, guides
  docs/       design, roadmap, user and developer documentation
```

`engine/` and `library/` build independently. The engine reads library files;
the library validator does not depend on the engine.

## Key decisions

- Language: Rust (decision 2026-11-27, replaces the earlier choice of C++20).
  The engine taken over from csim is C++ and serves as the reference until
  the Rust rewrite reaches the same results and speed
  ([RUST_MIGRATION.md](../RUST_MIGRATION.md)). Reasons: one build tool on all
  platforms, memory safety for parsers of untrusted input, WebAssembly.
- Build: cargo (target); CMake + Ninja for the C++ reference until it is removed.
- Library format: TOML with a published schema. Readable by people and
  language models, unambiguous to validate.
- Error messages: every diagnostic has a stable code (e.g. `E0412`), a one-line
  message with file, line and column, and a long explanation available via
  `mspice explain E0412`. All diagnostics are also available as JSON.
- Progress: long runs report progress as JSON lines on request
  (`--progress=json`), so GUIs, terminals and agents can show it.
- Multi-core: parameter sweeps, Monte Carlo and variants run in parallel.

## KiCad files

MSpice must never damage a user's design:

- KiCad files (`.kicad_sch`, `.kicad_pcb`, `.kicad_pro`) are opened read-only.
- Everything MSpice needs to store (load profiles, probes, simulation
  settings) goes into a sidecar file next to the project,
  `<project>.mspice.toml`.
- If writing to KiCad files ever becomes necessary, it requires a lossless
  round-trip parser and a test that an unmodified read/write cycle produces a
  byte-identical file.

## Trust in models (stage 5)

- Each model has a `quality` level: `datasheet`, `measured`,
  `vendor-verified`.
- Each model ships reference curves; CI simulates them and fails on deviation.
- Models can be signed (Ed25519). A signature by a registered vendor key shows
  the model as vendor-verified. Signatures detect any change.
- Optional encryption (stage 10) is documented honestly as obfuscation: an open
  program has to contain the key.

## Name

"MSpice" (M for multi: multi-core, multiple front ends). Research on
2026-11-27 found no active trademark in EU/DE/US; earlier, expired US marks
from EDA companies exist. Before release: direct search in DPMAregister,
EUIPO eSearch and WIPO; README disclaimer that MSpice is not affiliated with
the owners of PSpice, HSPICE or LTspice.

## Left to the builder

Internal module structure, test framework choice, exact TOML keys (within the
spec process in `library/`), CI details.
