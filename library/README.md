# MSpice part library

Open part library for the MSpice circuit simulator: the file format, a
validator, generic parts, load models and guides.

| Path | Content |
|---|---|
| [SPEC.md](SPEC.md) | Format specification 0.1 (TOML) |
| [schema/](schema/) | JSON Schemas of part files and manifests, for editors and tools |
| [library.toml](library.toml) | Manifest of the generic library |
| [parts/](parts/) | Generic standard parts (`generic_*`) |
| [parts/loads/](parts/loads/) | Consumers: addressable LED, 1-Wire sensor, fan, motors, LED strip, template |
| [parts/noise/](parts/noise/) | Interference sources |
| [tools/libcheck/](tools/libcheck/) | Validator `mspice-libcheck` (C++20, standalone) |
| [tests/](tests/) | Positive (`valid/`), negative (`invalid/`) and merge tests of the validator |
| [docs/GUIDE_CREATE_PART.md](docs/GUIDE_CREATE_PART.md) | Writing a part by hand |
| [docs/AGENT_GUIDE.md](docs/AGENT_GUIDE.md) | Step-by-step guide for AI agents |
| [docs/MERGING.md](docs/MERGING.md) | Using several libraries together |
| [docs/ERRORS.md](docs/ERRORS.md) | Index of diagnostic codes |

## Build and run the validator

From the repository root (CMake 3.21+, Ninja, a C++20 compiler):

```sh
cmake -S library/tools/libcheck -B build/libcheck -G Ninja
cmake --build build/libcheck
ctest --test-dir build/libcheck --output-on-failure

build/libcheck/mspice-libcheck check library/
build/libcheck/mspice-libcheck explain E0004
build/libcheck/mspice-libcheck check --format=json library/parts/generic_resistor.toml
build/libcheck/mspice-libcheck merge -o out/ library other-library
```

| Command | Purpose |
|---|---|
| `check [--format=json] [--deny-warnings] <path>...` | Validate files, directories or manifests |
| `explain <code>` | Long explanation with a wrong and a correct example |
| `codes [--format=json]` | List all diagnostic codes |
| `merge [--on-conflict=error\|first\|last] [--force] -o <dir> <library>...` | Merge libraries |
| `selftest` | Check that every code is documented |

Exit codes: `0` no errors, `1` errors found (or warnings with
`--deny-warnings`), `2` usage error, `3` output could not be written.

Diagnostics look like this:

```
error[E0081]: unknown name `resistence` in `r` in [model] (`R`, resistor)
  --> library/parts/generic_resistor.toml:64:18
   |
64 | params = { r = "{resistence}" }
   |                  ^^^^^^^^^^
   = hint: did you mean `resistance`?
```

## Status of the parts

All parts are `quality = "draft"`: their values are textbook values, author
estimates or typical datasheet values that have not been checked against
one specific document revision. The engine does not simulate library parts
yet (roadmap stage 2); reference tests are checked for structure only.

| Group | Parts |
|---|---|
| Passive | `generic_resistor`, `generic_capacitor` (features `esr`, `esl`), `generic_inductor` (feature `dcr`) |
| Semiconductors | `generic_diode`, `generic_led`, `generic_zener`, `generic_npn`, `generic_pnp`, `generic_nmos`, `generic_pmos` |
| Analog and power | `generic_opamp`, `generic_comparator`, `generic_ldo_regulator`, `generic_switch` |
| Loads | `generic_rgb_led_addressable`, `generic_temp_sensor_1wire`, `generic_fan_4pin`, `generic_motor_dc`, `generic_motor_bldc`, `generic_motor_stepper`, `generic_led_strip`, `generic_load_custom` |
| Noise | `generic_noise_ripple`, `generic_noise_burst` |

## Licences

- Part data (`parts/`, `tests/` fixtures, `library.toml`): CC BY 4.0, see
  [LICENSE](LICENSE), unless a file states otherwise.
- Validator code (`tools/`): Apache-2.0 with Commons Clause, see the
  repository [LICENSE](../LICENSE).
- Third-party code: see [THIRD_PARTY.md](THIRD_PARTY.md).

Product names in `compatible` fields are trademarks of their owners and are
used only to describe compatibility.
