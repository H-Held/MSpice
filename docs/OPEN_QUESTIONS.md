# Open questions and gaps

Things that are not decided, not specified or not verified yet, across all
MSpice repositories. Each item names who decides or which task closes it.
Remove an item when it is resolved and note the decision in the relevant
document.

## Decisions for the maintainers

| # | Question | Proposal | Affects |
|---|---|---|---|
| D1 | Who owns the KiCad fork repository (professor or H-Held), and its name | fork of the GitHub mirror under the account that does most of the work; GitLab fork only for upstream merge requests | KICAD_FORK.md |
| D2 | KiCad base version of the fork | latest 10.0.x tag, cherry-picks from the development branch for schematic IPC and export jobs | KICAD_FORK.md |
| D3 | Repository for `mspice-kicad` and its language | own repository under the MSpice licence, Rust (protobuf with `prost`, transport with an `nng` binding, both permissively licensed) | INT-10 |
| D4 | Company prefix for library ids | `<company>_*` next to `generic_*` | library SPEC, VERSIONING |
| D5 | GUI toolkit of mspice-studio | `egui` with `wgpu` | mspice-studio |
| D6 | Language of `mspice-emu/bridge` | Rust with a C ABI for the QEMU device code | mspice-emu |
| D7 | Default AI provider and model in the chat, cost limits per session | no default; user chooses; configurable token/cost limit with warning | KICAD_FORK.md section 9 |
| D8 | First release: when to tag v0.1.0 and what it contains | after RST-02 (validator in Rust) with the C++ engine still as reference | VERSIONING |
| D9 | Documentation hosting (GitHub Pages or only Markdown in the repository) | Markdown first, Pages once the API exists | - |
| D10 | Status of the old internal tickets (013 superseded, tickets in review) | close or mark superseded | internal repository |

## Specifications not written yet

| Gap | Closed by |
|---|---|
| Engine API (C ABI, JSON-RPC methods, error codes) | INT-14 |
| Co-simulation protocol | SIL-00 |
| Project sidecar file format `<project>.mspice.toml` (devices, probes, tests) | INT-09 |
| Event format of the activity stream (complete list of kinds and fields) | INT-11 |
| MCP tool definitions (names, arguments, results, permission level) | INT-10, INT-15 |
| Mapping of library parts to KiCad `Sim.*` symbol fields | INT-01 |
| Golden reference data set for the Rust migration (which circuits, which machine) | RST-03 |
| Thermal coupling between parts (LED and driver on one heat sink) | not planned yet; candidate for a new LIB/ENG task |
| Profiles shipped with the library (solar day, wind, drive cycle): source data must be under an open licence | LIB-08 |

## Not verified yet

| Item | How to verify |
|---|---|
| CI on GitHub (Linux GCC/Clang, Windows MSYS2/MSVC) | look at the Actions tab, fix failures |
| KLU build path | build with `-DMSPICE_USE_KLU=ON` on a machine with SuiteSparse |
| Values of all 24 library parts (all `draft`) | compare against cited datasheets, then raise `quality` |
| Whether KiCad would accept an upstream event-subscription API | ask on the KiCad developer list before writing the patch |
| Licence model (Apache-2.0 + Commons Clause) | lawyer or HTW legal office before the first release |
| Trademark "MSpice" | direct search in DPMAregister, EUIPO eSearch, WIPO Global Brand Database |

## Repository settings to do on GitHub

- Branch protection for `main` and `dev`: pull requests required, CI must
  pass, no force-push.
- Private vulnerability reporting on (SECURITY.md refers to it).
- DCO app installed.
- Invite Lucas Roy David (`Livadlol`) as collaborator.
- Create `mspice-emu` and `mspice-studio` on GitHub and push.
