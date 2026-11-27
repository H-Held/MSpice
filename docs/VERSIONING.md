# Versioning and compatibility

MSpice is split over several repositories. This document defines how their
versions relate, so that any combination a user installs either works or says
clearly why not.

## Repositories

| Repository | Content | Licence |
|---|---|---|
| MSpice | engine, CLI, library format and generic parts, protocol and API specifications | Apache-2.0 + Commons Clause; parts CC BY 4.0 |
| mspice-studio | standalone GUI (Rust) | Apache-2.0 + Commons Clause |
| mspice-emu | emulator endpoints (ESP32, AVR, STM32) | GPL-2.0-or-later (simavr/ GPL-3.0-or-later) |
| KiCad fork + mspice-kicad | editor integration, API/MCP server, AI chat | fork GPLv3; mspice-kicad under the MSpice licence |

## What carries a version

| Item | Scheme | Where it is declared | Breaking change means |
|---|---|---|---|
| Each repository release | SemVer `MAJOR.MINOR.PATCH` | git tag `vX.Y.Z`, `Cargo.toml` | removed or changed public behaviour |
| Library format | `format_version = "MAJOR.MINOR"` | every `library.toml` and part file | old files no longer valid |
| Co-simulation protocol | integer `protocol = N` | handshake message | old endpoints cannot talk |
| Engine API (C ABI, JSON-RPC) | `api = "MAJOR.MINOR"` | every response, `mspice --version --json` | removed method or changed meaning |
| A library (collection of parts) | SemVer | its `library.toml` | part removed, pin changed |

Before 1.0, a MINOR increase may break; this is stated in the changelog.

## Compatibility rules

- The engine reads every library format version with the same MAJOR and an
  equal or lower MINOR. Unknown newer MINOR fields give a warning, not an
  error; a newer MAJOR gives error E0003 with the supported range.
- Protocol: both sides send the versions they support in `hello`; the highest
  common one is used; no common version gives a clear error naming both
  ranges.
- API: clients send the API version they expect; the server answers with its
  own; a different MAJOR is refused with an error that names both.
- Each repository's README has a compatibility table: "mspice-studio 0.3
  works with MSpice 0.4 to 0.5 (api 1.x)".

## Combining libraries

Libraries from different sources (the generic library, a company library, a
project's own parts) are combined with `mspice-libcheck merge` or by listing
them as dependencies in a project's `library.toml`:

- Ids must be unique; a company prefix (for example `acme_*`) avoids clashes
  with `generic_*`.
- A conflict is only resolved by an explicit `[[overrides]]` entry, never
  silently.
- Each dependency names a version range; the validator checks it.
- The merged result records where each part came from.

## Releases

- Branches: `main` (released), `dev` (integration), feature branches; see
  AGENTS.md.
- A release is a tag on `main` with a changelog entry (`CHANGELOG.md`, Keep a
  Changelog style), built and tested by CI on Windows and Linux; binaries are
  attached to the GitHub release.
- Cross-repository changes (for example a new protocol message) are released
  in this order: specification in MSpice, MSpice implementation, then the
  other repositories, each declaring the new minimum version.
