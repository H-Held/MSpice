# MSpice in a KiCad-based editor: GUI, API, MCP and AI chat

Planning document for the KiCad fork with native simulation and AI
integration. The fork itself lives in its own repository under GPLv3 (like
KiCad); MSpice provides the engine, the part library and the interfaces
described here. Related tasks: INT-01 to INT-08, SIL-00 in [TASKS.md](../TASKS.md).

## 1. Goals

1. Simulate a design without leaving the editor: pick loads and sources,
   place probes, run, look at waveforms in an oscilloscope view.
2. Off-board devices (fans, motors, batteries, solar panels, ...) attached to
   connectors without changing the KiCad files.
3. An AI chat inside the editor that can read and change the design, run
   simulations, DRC and renders, with the provider chosen by the user.
4. The same functions available to external tools through a normal API and an
   MCP server.
5. Easy to follow new KiCad releases.

## 2. Keeping up with KiCad releases

Most features do not need to live inside KiCad's code:

- KiCad 9 introduced an IPC API (protobuf messages over a local socket, with
  official Python bindings), extended in later releases. It lets an external
  process read and edit schematics and boards while KiCad is running.
- Therefore the plan is **external first**: the simulation front end, the MCP
  server, the AI chat backend and the API run as a separate process
  (`mspice-kicad`, see section 3) that talks to KiCad through the IPC API and
  to MSpice through its API.
- The fork only carries what cannot be done from outside: a dockable panel that
  hosts the chat and the oscilloscope inside KiCad's window, menu entries, and
  missing IPC API calls.
- Rules for the fork:
  - Keep the patch set small and grouped by feature (one commit series per
    feature), rebased onto each KiCad release tag.
  - Every new IPC call is written so that it can be offered upstream; once
    accepted upstream, the fork drops its copy.
  - CI builds the fork against the latest release tag and against KiCad's
    nightly branch, so breaking upstream changes are seen early.
  - No KiCad file format changes. Everything MSpice needs is stored either in
    standard symbol fields or in the sidecar file.
- Result: a new KiCad version usually means rebasing a few patches; the
  external process keeps working as long as the IPC API is compatible.

### IPC coverage for schematic and PCB, and rendering images

Checked on 2026-11-27 against the API definitions in KiCad's repository
(`api/proto/`) and a local KiCad 10.0.6 installation:

| | KiCad 10.0 (current release) | KiCad development branch (future 11) |
|---|---|---|
| PCB editing | Full: `GetItems`, `CreateItems`, `UpdateItems`, `DeleteItems`, commits (undoable), selection, hit test, `SaveCopyOfDocument`, `SaveDocumentToString` | same, plus board rules and tasks |
| Schematic | Data types only; `schematic_commands.proto` contains no commands | `GetSchematicHierarchy`, `GetSchematicNetlist`, `PlaceSymbolFromLibrary`, schematic rules and tasks |
| Images through IPC | none | Jobs: `RunSchematicJobExportSvg/Pdf/Png`, `RunBoardJobExportSvg/Png/Pdf`, `RunBoardJobExportRender` (3D) |
| Images through `kicad-cli` | `sch export svg/pdf`, `pcb export svg/pdf` (per layer), `pcb render` (3D view to PNG/JPEG: side, zoom, rotation, quality) | same |

Plan, so that a model can see both the schematic and the routing:

1. **KiCad 10 path (works now):** `mspice-kicad` asks the PCB editor for a
   copy of the open board (`SaveCopyOfDocument` to a temporary folder), then
   calls `kicad-cli pcb export svg` (chosen layers, for routing) or
   `kicad-cli pcb render` (3D). For the schematic it uses the saved file with
   `kicad-cli sch export svg`, and reads its content read-only. SVG is turned
   into PNG inside `mspice-kicad` (`resvg`, permissive licence). The user's
   files are never written.
2. **Fork on KiCad 10:** cherry-pick the upstream schematic commands and the
   export jobs from KiCad's development branch into `mspice/10.0`. Then the
   live, unsaved schematic and board can be read and rendered through IPC.
   These patches disappear again when the fork moves to KiCad 11, because
   they are upstream code.
3. **KiCad 11 path:** use the IPC jobs directly; the `kicad-cli` path stays as
   a fallback for headless use and CI.

MCP tools built on this: `render_schematic(sheet, format)`,
`render_board_layers(layers, area, scale)`, `render_board_3d(side, rotation,
zoom)`, each returning an image plus the KiCad version and path used.

### Upgrade procedure for a new major version (example: 10 to 11)

| Part | How it is kept up to date |
|---|---|
| Fork | One branch per KiCad major version: `mspice/10.0` is based on the tag `10.0.x`. For 11: create `mspice/11.0` with `git rebase --onto 11.0.0 10.0.<last> mspice/10.0`, fix conflicts, run CI. |
| Own code in the fork | Lives in its own directory (`mspice/`) with its own CMake subdirectory; it only uses public KiCad classes. |
| Hooks into KiCad code | Kept to a few touch points (panel registration, menu entries, missing IPC calls), each listed in `PATCHES.md` with file, reason and upstream status. Only these can conflict. |
| `mspice-kicad` | Talks to KiCad through the IPC API only. A thin adapter per KiCad major version (`kicad_ipc::v10`, `v11`) and a capability check at start-up; unknown versions get a clear message instead of wrong behaviour. |
| Stored data | Unaffected: MSpice never writes KiCad files; symbol fields `Sim.*` and the sidecar file are independent of the KiCad file format version. |
| Early warning | CI builds the fork against KiCad's development branch every night, so changes for the next major version show up months before its release (KiCad publishes release candidates first). |
| Support policy | The current KiCad major version and the one before it. |

Expected effort per major version: rebasing the hook commits and adapting the
IPC adapter, typically days rather than weeks, provided the hook list stays
short and missing IPC calls are contributed upstream.

## 3. Architecture

```
+------------------------------- KiCad fork (GPLv3) --------------------------+
|  Schematic / PCB editor   |  dock panel: chat, oscilloscope, test results   |
|  (unchanged core)         |  (thin UI, talks to mspice-kicad)               |
+------------------+--------+----------------------+--------------------------+
                   | KiCad IPC API                 | local socket
+------------------v-------------------------------v--------------------------+
|  mspice-kicad (separate process)                                            |
|  - project model: reads schematic/board through IPC, reads sidecar file     |
|  - netlist builder: symbols + off-board devices -> MSpice circuit           |
|  - API server (JSON-RPC over local socket / HTTP on localhost)              |
|  - MCP server (stdio and HTTP) exposing the same functions as tools         |
|  - AI chat backend with pluggable providers                                 |
+------------------+------------------------------------+---------------------+
                   | MSpice API (C ABI / JSON-RPC)      | co-sim protocol (SIL-00)
            +------v------+                      +------v---------------+
            | MSpice      |                      | mspice-emu endpoints |
            | engine      |                      | (ESP32, AVR, STM32)  |
            +-------------+                      +----------------------+
```

Licence boundary: `mspice-kicad` talks to KiCad only over the IPC API, so it
can stay under the MSpice licence; code compiled into KiCad is GPLv3.

## 4. Where data is stored

| Data | Stored in | Why |
|---|---|---|
| Which MSpice part a symbol uses, its parameters | standard symbol fields `Sim.Device`, `Sim.Type`, `Sim.Params` (KiCad's own simulation fields; `Sim.Device = MSPICE`) | KiCad writes these fields itself, so files stay valid and other tools ignore them |
| Off-board devices (fan on J3, battery on J1), probes, stimulus profiles, test definitions, run settings | `<project>.mspice.toml` next to the project | never touches KiCad files; easy to diff and merge |
| Results | `<project>-mspice/` output folder | can be deleted any time |

MSpice never writes `.kicad_sch` or `.kicad_pcb` directly; edits to the design
always go through KiCad's IPC API, so KiCad stays the only writer.

## 5. Load and source selection (user view)

A panel lists the library catalogue by category: loads (fan, LED, LED strip,
incandescent lamp, pump, DC/BLDC/hub/stepper/synchronous/asynchronous motor,
heater, relay), sources and storage (battery by chemistry, rechargeable
packs, supercapacitor, solar panel, wind turbine, hub generator, mains, lab
supply), interference (ripple, burst, surge, ESD, fast transients, magnetic
field sources). See [library/docs/CATALOG_PLAN.md](../../library/docs/CATALOG_PLAN.md).

```
+- Add device ------------------------------------------------+
| Category: [Sources > Battery          v]                    |
| Part:     generic_battery_li_ion   preset: [LFP v]          |
| Attach:   (o) connector J1  pins: + -> J1.1   - -> J1.2     |
|           ( ) schematic symbol                              |
| Cells: [4s1p]  Capacity: [10 Ah]  SoC at start: [80 %]      |
| Temperature: [constant v] [25 degC]                         |
| Features: [x] thermal  [ ] aging  [x] protection limits     |
|                                  [Validate] [Add] [Cancel]  |
+-------------------------------------------------------------+
```

- Each device can be added any number of times, on any board of a
  multi-board project.
- Every parameter shows its unit, range and default; invalid values are
  rejected with the library validator's error code and hint.
- Driving quantities (irradiance, wind, torque, temperature) offer the
  stimulus modes constant, steps, ramp, curve file, sine, profile and random.
- Parameters can be marked as sweep variables (list, range, Monte Carlo
  tolerance).

## 6. Oscilloscope and analysis view

- Probes on nets (voltage), on pins (current), differential probes, and
  device quantities (motor speed, battery SoC, LED temperature).
- Scope-like controls: time base, channels with scale and offset, trigger
  (edge, level, pulse width), cursors, measurements (frequency, duty, rise
  time, RMS, peak), math channels, FFT.
- Progress while the simulation runs (MSpice `--progress=json`), live
  waveform update, stop button.
- Limit violations (ENG-15) shown on the schematic and in a list.
- Fast rendering of long runs: min/max decimation per pixel column.

## 7. Firmware in the loop

- A project can name a firmware per microcontroller symbol (ELF file or a
  build command, for example an ESP-IDF project).
- The run starts the matching mspice-emu endpoint (ESP32, AVR, STM32) and
  couples it through the co-simulation protocol (SIL-00).
- Shipped example: an ESP32 project that drives a 4-pin fan with PWM, reads the
  tach signal and an NTC through the ADC, and controls an addressable LED
  strip; with a test definition (section 8).

## 8. Definable tests

Tests are part of the project, stored in the sidecar file, runnable from the
GUI, the CLI (`mspice test project.mspice.toml`) and CI:

```toml
[[tests]]
name = "fan starts within 2 s"
stimulus = { battery_soc = 0.2, ambient = "40 degC" }
run = { analysis = "tran", stop = "5 s" }
expect = [
  { probe = "fan.speed", at = "2 s", min = "1500 rpm" },
  { probe = "v(VBUS)", over = "0..5 s", min = "11.0 V", max = "12.6 V" },
  { limit_violations = 0 },
]
```

Each test reports pass/fail with the measured value; failures link to the
waveform at the failing time.

## 9. AI chat with selectable providers

- Chat panel docked in the editor; the backend lives in `mspice-kicad`.
- Provider interface with adapters: Anthropic, OpenAI, Google, Mistral, and
  any OpenAI-compatible endpoint (local models through Ollama, LM Studio,
  vLLM). The user chooses provider and model in the settings.
- API keys are stored in the operating system's credential store, never in
  project files.
- The chat uses the same tool set as the MCP server, so behaviour is identical
  whether the model runs inside the editor or in an external MCP client
  (Claude Desktop, Claude Code, other MCP-capable tools).
- Every change the AI makes goes through the IPC API as a normal, undoable
  edit and is listed in the chat; destructive actions ask for confirmation.
- Works without any AI configured; the chat is optional.

## 10. API and MCP tools

The same functions are offered as JSON-RPC methods and as MCP tools:

| Group | Functions |
|---|---|
| Project | open, list boards, list symbols/nets/footprints, read sidecar |
| Schematic | add/move/delete symbol, connect, set field, annotate |
| PCB | place footprint, route track, add via, zone fill, set design rules |
| Checks | ERC, DRC (result as structured list with positions) |
| Render | schematic sheet as PNG/SVG, board 2D layers as PNG, 3D render as PNG, so a model can see the current routing |
| Library | search catalogue, show part, validate part, add part from a TOML text |
| Devices | add/remove off-board device, set parameters and stimulus |
| Simulation | run, progress, stop, read waveform (decimated or full), measurements |
| Tests | list, run, results |
| Firmware | set firmware, build, start co-simulation |

API versioning follows [VERSIONING.md](../VERSIONING.md); every response
carries the API version.

## 11. Which earlier V-tasks concern the KiCad side

The earlier task list used V-numbers. Those related to KiCad are now INT tasks:

| Old | New | Content |
|---|---|---|
| V37 | INT-01, INT-02 | KiCad project to MSpice without manual work, multi-board stacks |
| V28a/b | INT-03, INT-04 | trace parasitics from the `.kicad_pcb`, PEEC |
| V38 | INT-05 | cable model from geometry |
| V39 | INT-06 | mated connectors |
| V40 | INT-07 | automatic parasitic detection and crosstalk |
| V30a | ENG-15 (+ symbol field) | voltage limits per part, chosen in KiCad |

## 12. Order of work

1. MSpice Rust migration (RST-01 to RST-04), API surface defined with it.
2. `mspice-kicad` external process: project model through IPC API, netlist
   builder, JSON-RPC API, MCP server (read functions, run simulation, render).
3. Sidecar file with off-board devices and tests; CLI `mspice test`.
4. Fork: dock panel with oscilloscope view and chat; provider adapters.
5. Write functions through IPC (schematic, PCB edits), DRC/render tools.
6. Firmware in the loop once SIL-00/SIL-01 and mspice-emu E1/E2 exist.
