# Roadmap

Stages are worked on in order. A stage is done when its acceptance criteria
pass in CI on Windows and Linux. Design background: [design](design/2026-11-27-mspice-design.md).

| Stage | Content | Acceptance |
|---|---|---|
| 0 Foundation | Repository without old history, licences, README, CONTRIBUTING with DCO, CI on Windows and Linux. Engine taken over from `csim` without vendor-derived models and project data. | Engine builds on both platforms, all tests green, licence audit clean |
| 1 Library format | TOML format spec and schema, validator with error codes and `explain`, JSON diagnostics, generic standard parts (`generic_*`), merging libraries, features that can be switched on and off, guides for agents (datasheet to part, openly licensed SPICE model to part) | Every rule of the spec has a positive and a negative test |
| 2 Loads | Consumers with fixed value, CSV curve, formula or state machine, any pin count. Examples: addressable RGB LED, 1-Wire temperature sensor, 4-pin fan, DC/BLDC/stepper motor, LED strip. Noise and interference sources | Each example simulates against its reference curve |
| 3 Engine | Live progress (`--progress=json`), multi-core sweeps and Monte Carlo, fault injection (open, short, tolerance), SPICE netlist import, speed work carried over from csim (Newton, predictor, step control, pattern cache) | Benchmarks recorded per release; import lists every unsupported construct |
| 4 Integration API | Stable C API, JSON-RPC over stdio, read-only KiCad import with sidecar file | API tests; KiCad files unchanged after every test (hash check) |
| 5 Trust | Quality level per model, mandatory reference curves in CI, Ed25519 signatures, vendor keys | Tampered model is rejected in a negative test |
| 6 Studio | Native GUI (Dear ImGui), waveform viewer, library editor | - |
| 7 SIL / HIL | Firmware co-simulation through emulators running as separate processes (open protocol), hardware-in-the-loop over USB/serial | - |
| 8 Microcontrollers | ESP32, AVR, STM32 peripheral models | - |
| 9 Converter | Separate tool: LTspice schematic/model files to and from the library format, only for openly licensed, unencrypted input | - |
| 10 Extras | Optional model encryption (documented as obfuscation), WebAssembly build for a browser demo | - |

Carried over from the csim plan: the open speed tasks (V-list) and the SPICE
import work (L1) move into stage 3. The open tasks of all stages, with
acceptance criteria, are listed in [TASKS.md](TASKS.md).
