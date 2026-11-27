# MSpice part library format, version 0.1

Status: draft for review. Format version string: `"0.1"`.

This document is normative for the part library. Where it says "must", the
validator `mspice-libcheck` reports an error when the rule is broken; where it
says "should", it reports a warning. Each rule names its diagnostic code in
brackets, for example [E0004]. The long explanation of a code is printed by
`mspice-libcheck explain <code>` and stored in [docs/errors/](docs/errors/).

A machine-readable form of the data model is in
[schema/part.schema.json](schema/part.schema.json) and
[schema/library.schema.json](schema/library.schema.json) (JSON Schema draft
2020-12, applied to the TOML data after conversion to JSON). The schemas help
editors and language models; the validator is the reference, because many
rules (units, references, expressions, CSV files) cannot be written in JSON
Schema.

Contents:

1. [Files and encoding](#1-files-and-encoding)
2. [Library manifest](#2-library-manifest-librarytoml)
3. [Merging libraries](#3-merging-libraries)
4. [Part file](#4-part-file)
5. [Values, quantities and units](#5-values-quantities-and-units)
6. [Pins](#6-pins)
7. [Parameters](#7-parameters)
8. [Features](#8-features)
9. [Models](#9-models)
10. [Loads](#10-loads)
11. [State machines](#11-state-machines)
12. [Noise and interference sources](#12-noise-and-interference-sources)
13. [Curves and CSV data files](#13-curves-and-csv-data-files)
14. [Reference tests](#14-reference-tests)
15. [Expressions](#15-expressions)
16. [Mapping to simulation elements](#16-mapping-to-simulation-elements)
17. [Using a part in a circuit](#17-using-a-part-in-a-circuit)
18. [Versioning of this format](#18-versioning-of-this-format)

## 1. Files and encoding

- Part files and manifests are TOML 1.0, UTF-8, file extension `.toml`.
  A TOML syntax error is [E0001]; a file that cannot be read is [E0002].
- Every file starts with a licence comment, for part data
  `# SPDX-License-Identifier: CC-BY-4.0`. The validator does not read
  comments; the `license` key is what counts (section 4.2).
- Every file has a top-level `format_version = "0.1"`. A missing or
  unsupported version is [E0003].
- Keys are lower case with underscores. A key that is not defined by this
  specification is an error [E0004]; the validator suggests the closest known
  key. Unknown keys are never ignored silently.
- A required key that is missing is [E0005]. A value of the wrong TOML type
  (for example a string where a boolean is expected) is [E0006]. A string that
  must be one of a fixed set of words is [E0007].
- Keys that exclude each other (for example `type` and `part` on an element)
  must not both be set, and when one of them is required, exactly one must be
  set [E0072].

## 2. Library manifest (`library.toml`)

A library is a directory with a file `library.toml`:

```toml
# SPDX-License-Identifier: CC-BY-4.0
format_version = "0.1"

[library]
name = "mspice-generic"          # required, ^[a-z][a-z0-9_-]*$
version = "0.1.0"                # required, semantic version
description = "Generic parts"    # required
license = "CC-BY-4.0"            # required, default licence of the parts
authors = ["The MSpice Authors"] # optional
parts = ["parts/*.toml", "parts/loads/*.toml"]  # required, paths or globs
exclude = ["parts/drafts/*.toml"]               # optional

[[dependencies]]                 # optional, any number
name = "acme-sensors"            # library name expected in the target manifest
path = "../acme-sensors"         # directory or path to its library.toml
version = ">=1.2.0, <2.0.0"      # optional version requirement

[[overrides]]                    # optional, any number
id = "generic_temp_sensor_1wire" # part id that this library replaces
library = "acme-sensors"         # library whose part is replaced
reason = "Measured conversion current"   # required
```

Rules:

- `parts` entries are paths relative to the manifest, with `/` as separator.
  `*` matches any characters except `/`, `?` matches one character, `**`
  matches any number of directories. An entry without wildcards must name an
  existing file [E0002]. A pattern that matches no file is a warning [W0120].
- The library name follows the identifier rule above [E0012]; the version is
  a semantic version `MAJOR.MINOR.PATCH` with optional `-prerelease` [E0014].
- A dependency `path` must lead to a manifest [E0121] whose `name` equals the
  dependency `name` [E0121]. If `version` is given, the dependency version
  must satisfy it [E0122]. Version requirements are a comma-separated list of
  comparators `=`, `>`, `>=`, `<`, `<=`, `^` (same major version, at least the
  given version) and `~` (same major and minor version, at least the given
  version); all comparators must hold.
- Dependency cycles are errors [E0126].
- Part ids must be unique inside one library [E0011].

When `mspice-libcheck check` is given a directory, a `library.toml` in it (or
in any subdirectory) makes that subtree a library: its files are checked only
if the manifest lists them. Other `.toml` files are checked as loose part
files that are used together.

## 3. Merging libraries

Several libraries can be used together, either through `[[dependencies]]` or
with `mspice-libcheck merge a/library.toml b/library.toml -o out/`.

Order: libraries are ordered by dependency, dependencies first (depth first,
in the order they are listed). For `merge`, the command line order is used;
each later library is treated as depending on all earlier ones.

When two libraries define the same part id:

1. If the later library has an `[[overrides]]` entry for that `id` whose
   `library` names the earlier library, the later part replaces the earlier
   one. This is how a company library replaces a generic part.
2. Otherwise, if both part files have identical content (after normalising
   line endings), the part is kept once and a warning is given [W0124].
3. Otherwise the merge fails with a conflict [E0123]. The message names both
   files and both part versions. Fix it by adding an override, by renaming
   one part, or by removing one of them.

An `[[overrides]]` entry whose `id` is not defined by the named library is an
error [E0125]: it would hide a typo.

`mspice-libcheck merge` writes a new library to the output directory:
`library.toml` (name `merged`, version `0.0.0`, its `parts` list naming every
kept part file) and a copy of every kept part file and the data files it
uses, below a directory per source library (`out/<library-name>/...`). Part
files are copied byte for byte. The option `--on-conflict=error|first|last`
changes rule 3 for one run (`error` is the default).

The merged library has no `[[overrides]]`; the record of what was replaced is
printed to the terminal and stored in `out/MERGE_REPORT.json`.

## 4. Part file

One part per file. File name: the part id plus `.toml`, for example
`generic_resistor.toml` (recommended, not checked).

```toml
# SPDX-License-Identifier: CC-BY-4.0
format_version = "0.1"

[part]
id = "generic_resistor"
name = "Resistor"
version = "0.1.0"
category = "passive/resistor"
description = "Ideal linear resistor with optional temperature coefficient."
compatible = []
author = "The MSpice Authors"
license = "CC-BY-4.0"
quality = "draft"
tags = ["passive"]

[part.source]
kind = "textbook"
title = "Ohm's law; temperature coefficient as in common SPICE resistor models"

[[pins]]
name = "p"
role = "analog"

[[pins]]
name = "n"
role = "analog"

[[parameters]]
name = "resistance"
type = "real"
unit = "Ohm"
default = "1 kOhm"
min = "1 mOhm"
tolerance = { relative = 0.01, distribution = "uniform" }

[model]
kind = "primitive"
element = "R"
nodes = ["p", "n"]
params = { r = "{resistance}" }

[[tests]]
name = "ohms_law"
analysis = "op"
[[tests.stimulus]]
name = "v1"
nodes = ["p", "n"]
source = "voltage"
waveform = "dc"
value = "1 V"
[[tests.expect]]
probe = "I(p)"
value = "1 mA"
rel_tol = 1e-6
```

### 4.1 Top-level keys

| Key | Type | Required | Meaning |
|---|---|---|---|
| `format_version` | string | yes | `"0.1"` |
| `part` | table | yes | identity and provenance |
| `pins` | array of tables | yes | at least one pin |
| `parameters` | array of tables | no | user-visible parameters |
| `features` | array of tables | no | optional behaviours |
| `curves` | array of tables | no | CSV-based curves |
| `model` | table | yes | how the part is simulated |
| `tests` | array of tables | no (warning [W0110] if absent) | reference tests |

### 4.2 `[part]`

| Key | Type | Required | Rule |
|---|---|---|---|
| `id` | string | yes | `^generic_[a-z0-9_]+$` [E0010]. Never a vendor part number. |
| `name` | string | yes | Short human-readable name. |
| `version` | string | yes | Semantic version of this part file [E0014]. Increase it when the model changes. |
| `category` | string | yes | Lower-case path, `^[a-z0-9_]+(/[a-z0-9_]+)*$` [E0012]. Top levels in use: `passive`, `semiconductor`, `analog`, `power`, `switch`, `load`, `motor`, `sensor`, `led`, `noise`. |
| `description` | string | yes | What the model covers and what it leaves out. |
| `compatible` | array of strings | no | Real part numbers this model approximates, e.g. `["WS2812B"]`. Only here may vendor names appear. |
| `author` | string | yes | Person or organisation that wrote the file. |
| `license` | string | yes | SPDX identifier. Accepted for part data: `CC-BY-4.0`, `CC0-1.0` [E0021]. |
| `quality` | string | yes | `draft`, `datasheet`, `measured`, `vendor-verified` [E0007], see 4.4. |
| `tags` | array of strings | no | Free search words. |
| `source` | table | yes | Where the values come from, see 4.3. |

### 4.3 `[part.source]`

| Key | Type | Required | Meaning |
|---|---|---|---|
| `kind` | string | yes | `datasheet`, `application_note`, `standard`, `measurement`, `spice_model`, `textbook`, `estimate` |
| `title` | string | yes | Document title, or a short description for `textbook`/`estimate` |
| `vendor` | string | for `datasheet`, `application_note`, `spice_model` | Publisher of the document |
| `document_id` | string | no | Vendor document number |
| `revision` | string | for quality `datasheet` | Revision or date printed on the document |
| `section` | string | for quality `datasheet` | Section, table or figure the values come from |
| `date` | string | for `measurement` | ISO date `YYYY-MM-DD` of the measurement |
| `url` | string | no | Where the document can be found. Do not copy it into the repository. |
| `model_license` | string | for `spice_model` | SPDX identifier of the SPICE model the values were taken from |
| `notes` | string | no | Anything a reviewer should know |

A `source` without `title`, or without a field required for its `kind`, is
[E0020]. For `kind = "spice_model"`, `model_license` must be an openly
licensed SPDX identifier (`CC0-1.0`, `CC-BY-4.0`, `MIT`, `BSD-2-Clause`,
`BSD-3-Clause`, `Apache-2.0`, `Zlib`) [E0021]. Vendor models with proprietary
terms must never be converted into library parts.

### 4.4 Quality levels

| Level | Meaning | Extra requirements [E0022] |
|---|---|---|
| `draft` | Plausible values, not checked against a single document. | none |
| `datasheet` | Every value is traceable to one datasheet revision. | `source.kind` is `datasheet`, `application_note` or `standard`; `source.vendor` (not for `standard`), `source.revision`, `source.section`; at least one `[[tests]]` |
| `measured` | Values were measured on real parts. | `source.kind = "measurement"`, `source.date`, at least one `[[tests]]` |
| `vendor-verified` | Confirmed by the manufacturer. | Not available before signatures exist (roadmap stage 5); always an error in format 0.1. |

## 5. Values, quantities and units

### 5.1 Value forms

Wherever this specification says *value*, three forms are accepted:

| Form | Example | Meaning |
|---|---|---|
| TOML number | `1000`, `4.7e-6` | In the unit of the field, no prefix |
| Quantity string | `"4.7 kOhm"`, `"100mA"`, `"10k"` | Number, optional SI prefix, optional unit |
| Expression string | `"{resistance * 2}"` | Expression (section 15) in braces |

Fields that are always expressions (`expr`, `probe`, `input`, `on`, `when`,
`actions`) take the expression without braces.

### 5.2 Quantity strings

Grammar: `number [space] [prefix] [unit]`, where `number` is a decimal number
with optional exponent. The SI prefixes are case sensitive:

| `f` | `p` | `n` | `u` or `µ` | `m` | `k` | `M` | `G` | `T` |
|---|---|---|---|---|---|---|---|---|
| 1e-15 | 1e-12 | 1e-9 | 1e-6 | 1e-3 | 1e3 | 1e6 | 1e9 | 1e12 |

Here `M` means mega and `m` means milli. This differs from SPICE netlists and
from expressions (section 15), where suffixes are case insensitive and `meg`
means mega. Quantity strings carry units, so the SI rule is the safe one.

The text after the number is first compared with the unit list as a whole,
then as prefix plus unit, then as a bare prefix. `"5 m"` is therefore five
metres, not five milli-units. A string that does not fit the grammar is
[E0032].

If a unit is written, it must equal the unit of the field [E0031]. Prefixes
are applied; units are not converted (a value in `degC` is not converted to
`K`). A value without unit is taken in the unit of the field.

### 5.3 Units

Units are written exactly as in this list [E0030]. Use `"1"` for
dimensionless values.

| Unit | Quantity | Unit | Quantity |
|---|---|---|---|
| `1` | dimensionless | `%` | percent (value 50 means 50 %) |
| `V` | voltage | `A` | current |
| `Ohm` | resistance | `S` | conductance |
| `F` | capacitance | `H` | inductance |
| `W` | power | `J` | energy |
| `C` | charge | `s` | time |
| `Hz` | frequency | `K` | temperature or temperature difference |
| `degC` | temperature | `m` | length |
| `kg` | mass | `N` | force |
| `N*m` | torque | `rad` | angle |
| `rad/s` | angular speed | `rpm` | revolutions per minute |
| `kg*m^2` | moment of inertia | `V*s/rad` | back-EMF constant |
| `N*m/A` | torque constant | `N*m*s/rad` | viscous friction |
| `Ohm/m` | resistance per length | `A/m` | current per length |
| `1/m` | count per length | `V/s` | slew rate |
| `V/sqrt(Hz)` | voltage noise density | `A/sqrt(Hz)` | current noise density |
| `K/W` | thermal resistance | `J/K` | heat capacity |
| `V/K` | voltage temperature coefficient | `1/K` | linear temperature coefficient |
| `1/K^2` | quadratic temperature coefficient | `A/V^2` | MOSFET transconductance parameter |
| `1/V` | channel-length modulation | `sqrt(V)` | body effect |
| `bit/s` | data rate | `dB` | ratio in decibel |

A value that is outside `min`/`max`, or negative where only positive values
make sense (pin limits, tolerances), is [E0033]. `min` greater than `max` is
[E0034].

## 6. Pins

```toml
[[pins]]
name = "vdd"                 # required, ^[A-Za-z0-9_]+$
role = "power"               # required
description = "Supply"       # optional
reference = "gnd"            # optional, pin the voltage limits refer to
abs_max_voltage = "6 V"      # optional
abs_min_voltage = "-0.5 V"   # optional
abs_max_current = "50 mA"    # optional, magnitude
driver = "push_pull"         # optional, outputs only
```

- Any number of pins (at least one). Names are unique within the part [E0013]
  and follow `^[A-Za-z0-9_]+$` [E0012]. Order matters: an instance that lists
  nodes as an array connects them in pin order.
- `role`: `power`, `ground`, `input`, `output`, `bidirectional`, `analog`
  [E0007].
- `driver` (for `output` and `bidirectional`): `push_pull`, `open_drain`,
  `tristate` [E0007].
- `reference` must name another pin [E0040]. Without `reference`, voltage
  limits refer to the first pin with role `ground`, or to the simulation
  ground if there is none.
- `abs_max_voltage`, `abs_min_voltage` are values in `V`; `abs_max_current`
  is a non-negative value in `A` [E0033]. These are limits, not operating
  conditions; the engine will report a violation during simulation
  (stage 3). Expressions are not allowed here [E0006].
- Every pin should be used by the model [W0041].

## 7. Parameters

```toml
[[parameters]]
name = "capacitance"             # required, ^[a-z][a-z0-9_]*$
type = "real"                    # required: real, integer, boolean, string, enum
unit = "F"                       # required for real and integer
default = "100 nF"               # required
min = "1 pF"                     # optional, real and integer
max = "10 mF"                    # optional, real and integer
adjustable = true                # optional, default true
choices = ["x7r", "c0g"]         # required for enum, not allowed otherwise
tolerance = { relative = 0.1, distribution = "normal", sigma = 3 }
feature = "esr"                  # optional, parameter only used with this feature
description = "Nominal capacitance"
```

- Names follow `^[a-z][a-z0-9_]*$` and must not be a reserved word (built-in
  variable or function name, section 15) [E0012]; unique [E0013].
- `default`, `min`, `max` are values (section 5) and must fit the type:
  integers for `integer`, `true`/`false` for `boolean`, one of `choices` for
  `enum` [E0006, E0007]. Expressions are not allowed in `default`, `min` and
  `max`.
- `default` must lie within `min`/`max` [E0033]; `min` ≤ `max` [E0034].
- `adjustable = false` marks a parameter that describes the part and must not
  be changed per instance (for example a fixed internal reference voltage). An
  instance or test that sets it is [E0050].
- `tolerance` describes manufacturing spread for Monte Carlo analysis
  (roadmap stage 3). Exactly one of `relative` (fraction, `0.05` = 5 %) or
  `absolute` (value in the parameter unit) [E0072]; non-negative [E0035].
  `distribution`: `uniform` (default) or `normal`; for `normal`, `sigma`
  (default 3) states how many standard deviations the tolerance covers
  [E0035]. Only `real` and `integer` parameters may have a tolerance
  [E0035].

## 8. Features

Features are optional behaviours. They are switched per part (`default`) and
per instance (section 17).

```toml
[[features]]
name = "esr"                      # required, ^[a-z][a-z0-9_]*$
description = "Series resistance" # required
default = false                   # optional, default false
requires = []                     # optional, other features
```

Common names, to be used where they fit: `thermal` (self-heating and
temperature dependence), `parasitics` (package capacitance and inductance),
`noise` (noise sources), `esr`, `esl`, `inrush`, `saturation`.

- Elements, outputs, branches and parameters may carry `feature = "name"`.
  They are only part of the model when the feature is enabled.
- A disabled two-terminal element is removed (`when_disabled = "open"`,
  default) or replaced by a connection between its nodes
  (`when_disabled = "short"`, for series parasitics such as ESR) [E0042 if
  the element has not exactly two nodes].
- References to undefined features are errors [E0060]. `requires` must not
  form a cycle [E0061]. Enabling a feature enables everything it requires.

## 9. Models

The `[model]` table has a `kind`:

| `kind` | Use | Required keys | Optional keys |
|---|---|---|---|
| `primitive` | one built-in element | `element`, `nodes` | `params`, `polarity` |
| `subcircuit` | network of elements and other parts | `elements` | `internal_nodes`, `signals` |
| `behavioral` | outputs given by expressions | `outputs` | `internal_nodes`, `elements`, `signals` |
| `load` | consumer, section 10 | `branches` | `internal_nodes`, `elements`, `signals`, `outputs`, `state_machine` |
| `noise_source` | interference generator, section 12 | `noise` | none |

A key not allowed for the kind is [E0004].

### 9.1 Nodes

Nodes inside a model are: the part's pin names, the names in
`internal_nodes` (`^[A-Za-z0-9_]+$`, unique, not equal to a pin name
[E0013]), and `0`, the global simulation ground. Any other node name is
[E0040].

### 9.2 Primitive elements

| `element` | Nodes (in order) | Parameters (required in bold) |
|---|---|---|
| `R` resistor | `p n` | **`r`** (Ohm), `tc1` (1/K), `tc2` (1/K^2) |
| `C` capacitor | `p n` | **`c`** (F), `ic` (V) |
| `L` inductor | `p n` | **`l`** (H), `ic` (A) |
| `D` diode | `a k` | `is` (A), `n`, `rs` (Ohm), `bv` (V), `ibv` (A), `cjo` (F), `vj` (V), `m`, `tt` (s), `eg` (V), `xti` |
| `Q` bipolar transistor | `c b e` | `is` (A), `bf`, `br`, `nf`, `nr`, `vaf` (V), `var` (V), `ikf` (A), `ikr` (A), `ise` (A), `ne`, `rb` (Ohm), `re` (Ohm), `rc` (Ohm), `cje` (F), `vje` (V), `mje`, `cjc` (F), `vjc` (V), `mjc`, `tf` (s), `tr` (s) |
| `M` MOSFET (level 1) | `d g s` or `d g s b` | `vto` (V), `kp` (A/V^2), `lambda` (1/V), `gamma` (sqrt(V)), `phi` (V), `rd` (Ohm), `rs` (Ohm), `cgso` (F), `cgdo` (F), `cbd` (F), `cbs` (F), `w` (m), `l` (m), `is` (A) |
| `SW` voltage-controlled switch | `p n cp cn` | `ron` (Ohm), `roff` (Ohm), `vt` (V), `vh` (V) |
| `V` voltage source | `p n` | `dc` (V) |
| `I` current source (flows from `p` through the source to `n`) | `p n` | `dc` (A) |
| `E` voltage-controlled voltage source | `p n cp cn` | **`gain`** |
| `G` voltage-controlled current source | `p n cp cn` | **`gm`** (S) |
| `B` behavioral source (only in `elements`) | `p n` | none; uses `source` and `expr` |

`Q` and `M` require `polarity`: `npn`/`pnp` for `Q`, `nmos`/`pmos` for `M`
[E0005, E0007]. Parameter names are lower case as listed; an unknown name is
[E0070] (with suggestion), a missing required one [E0071]. A wrong number of
nodes is [E0042]. Parameter values may be expressions; units of quantity
strings are checked against the table [E0031]. Parameters not given take the
usual SPICE defaults.

```toml
[model]
kind = "primitive"
element = "D"
nodes = ["a", "k"]
params = { is = "{i_s}", n = "{n_ideality}", rs = "{r_series}" }
```

### 9.3 Elements (`[[model.elements]]`)

Used by `subcircuit`, `behavioral` and `load`:

| Key | Type | Meaning |
|---|---|---|
| `name` | string | required, `^[A-Za-z][A-Za-z0-9_]*$`, unique in the part [E0012, E0013] |
| `type` | string | a primitive element letter from 9.2 (exclusive with `part`) [E0072] |
| `part` | string | id of another library part (exclusive with `type`) [E0073] |
| `nodes` | array or table | array: nodes in element/pin order; table (only with `part`): pin name to node |
| `params` | table | element parameters (with `type`) or instance parameters (with `part`) [E0070, E0075] |
| `features` | table | only with `part`: feature switches of the instance, `{ thermal = true }` [E0060] |
| `polarity` | string | for `Q` and `M` |
| `source` | string | for `B`: `voltage` or `current` |
| `expr` | string | for `B`: expression |
| `feature` | string | element only present when the feature is enabled |
| `when_disabled` | string | `open` (default) or `short` |

A part must not contain itself, directly or through other parts [E0074]. An
instance of another part must connect all of its pins [E0042]; pins not
listed in a table-form `nodes` are an error, so that nothing is left open by
accident.

### 9.4 Signals (`[[model.signals]]`)

Named internal quantities, usable in all expressions of the part:

| Key | Type | Meaning |
|---|---|---|
| `name` | string | required, `^[a-z][a-z0-9_]*$` |
| `expr` | string | required, expression |
| `tau` | value (s) | optional first-order low-pass time constant applied to the result |
| `unit` | string | optional, documentation and checking of the unit name [E0030] |
| `description` | string | optional |

Example: the duty cycle of a PWM input, `expr = "V(pwm, gnd) > 1.4 ? 100 : 0"`,
`tau = "5 ms"`.

### 9.5 Outputs (`[[model.outputs]]`)

Used by `behavioral` and `load`:

| Key | Type | Meaning |
|---|---|---|
| `name` | string | required |
| `source` | string | required: `voltage`, `current`, `switch` |
| `nodes` | array of 2 node names | required; for `voltage` the first is positive, for `current` the current flows from the first node through the source to the second, for `switch` the switch connects the two |
| `expr` | string | required; for `switch` a condition (non-zero means closed) |
| `r_out` | value (Ohm) | `voltage` only: series output resistance |
| `ron`, `roff` | value (Ohm) | `switch` only, defaults 1 Ohm and 1 GOhm |
| `feature` | string | optional |

## 10. Loads

A load describes a consumer: something that draws current from its supply
pins, possibly depending on time, inputs and an internal protocol.

```toml
[model]
kind = "load"

[[model.branches]]
name = "supply"
nodes = ["vdd", "gnd"]      # current flows from the first node into the load and out of the second
quantity = "current"        # current (A), resistance (Ohm) or power (W); default current
mode = "fixed"              # fixed, curve, formula, state_machine
value = "{i_idle}"
```

| `mode` | Required keys | Meaning |
|---|---|---|
| `fixed` | `value` | constant value (a value, may be an expression of parameters) |
| `curve` | `curve`, `input` | look up curve `curve` (section 13) at `x = input` |
| `formula` | `expr` | any expression, re-evaluated at every time step |
| `state_machine` | none (`value` optional as fallback) | each state of `[model.state_machine]` sets the value in its `values` table |

Optional keys of a branch: `feature`, `description`, `v_min` (value in V,
only for `quantity = "power"`: below this voltage the load behaves as a
resistor `v_min^2 / P` so that start-up converges; default `0.5 V`).

A load may also contain `elements` (for example the rail resistance of an LED
strip), `signals`, `outputs` (for example a tachometer output) and a
`state_machine`. A branch with `mode = "state_machine"` needs
`[model.state_machine]` [E0005]. A key that belongs to another mode is
[E0004]. A `curve` name that is not defined is [E0094].

## 11. State machines

A state machine describes digital behaviour: protocol decoding, conversion
phases, timing. It is evaluated by the engine as an event-driven process
next to the analog solution (section 16).

```toml
[model.state_machine]
initial = "idle"

[[model.state_machine.variables]]
name = "bit_count"
init = 0

[[model.state_machine.inputs]]
name = "din"                       # logic input usable in expressions (0 or 1)
expr = "V(din, gnd)"               # analog quantity that is compared
high = "{0.7 * v_supply}"          # rising threshold
low = "{0.3 * v_supply}"           # falling threshold (hysteresis)

[[model.state_machine.states]]
name = "idle"
values = { supply = "{i_quiescent}" }  # branch values while in this state
on_enter = ["bit_count = 0"]

[[model.state_machine.states]]
name = "high"

[[model.state_machine.transitions]]
from = "idle"                     # state name or "*" for every state
to = "high"
on = "rising(din)"                # event: rising(input) or falling(input)
```

Keys:

- `initial` (required): name of the start state [E0100].
- `variables`: `name` (`^[a-z][a-z0-9_]*$`), `init` (number, required),
  `description`. Variables are real numbers.
- `inputs`: `name`, `expr` (required), `high` and `low` (required values;
  `low` ≤ `high` is not checked when expressions are used).
- `states`: `name` (required, unique [E0013]), `values` (table: branch or
  output name to value [E0103]), `on_enter` and `on_exit` (arrays of
  actions), `final` (boolean, default false), `description`.
- `transitions`: `from`, `to` (required, state names or `"*"` for `from`
  [E0100]), and at least one trigger [E0072]:
  - `on`: an event `rising(x)` or `falling(x)` of an input `x`;
  - `after`: a value in seconds, the transition fires when the machine has
    been in the `from` state for this time;
  - `when`: a condition, checked at every accepted time step.
  When several triggers are given, all must hold (`on` and `when`: the
  condition is checked at the event; `after` and `when`: the condition is
  checked once the time has passed). Optional `guard` (condition, same as
  adding it to `when`), `actions` (array), `priority` (integer, higher first;
  default 0; among equal priorities the first listed wins).
- Actions are assignments `variable = expression`; the target must be a
  declared variable [E0081].
- In state machine expressions the following are also available: variables,
  input names (logic value 0 or 1), `time_in_state` (seconds),
  `in_state(name)` (1 if the machine is in that state).

Checks: every state must be reachable from `initial` [E0101]; a state with no
outgoing transition that is not marked `final = true` is a warning [W0102].

## 12. Noise and interference sources

```toml
[model]
kind = "noise_source"

[model.noise]
type = "ripple"            # white, pink, burst, ripple, esd
output = "voltage"         # voltage or current
nodes = ["p", "n"]
amplitude = "{amplitude}"
frequency = "{frequency}"
shape = "full_wave"
```

| `type` | Required keys | Optional keys | Meaning |
|---|---|---|---|
| `white` | `density`, `bandwidth` | `seed` | Gaussian white noise, spectral density in V/sqrt(Hz) or A/sqrt(Hz), band-limited |
| `pink` | `density`, `f_min`, `f_max` | `seed` | 1/f noise; `density` at 1 Hz |
| `burst` | `amplitude`, `rate`, `duration` | `seed`, `rise_time` | Random telegraph noise: steps of `amplitude`, mean `rate` events per second, mean `duration` |
| `ripple` | `amplitude`, `frequency` | `shape`, `offset`, `phase` | Periodic supply ripple; `shape`: `sine` (default), `triangle`, `sawtooth`, `full_wave`, `half_wave` |
| `esd` | `peak`, `rise_time`, `decay_time` | `delay`, `interval`, `count` | Double-exponential pulse similar in shape to ESD test pulses, repeated every `interval`, `count` times (default 1) |

Waveforms start at time 0 at phase 0: `sine` at the offset, rising; `triangle`
at the offset, rising, reaching `offset + amplitude` after a quarter period;
`sawtooth` rising from `offset - amplitude`; `full_wave` and `half_wave` are
rectified sines between `offset` and `offset + amplitude`. `phase` (rad)
shifts the start. Burst noise switches between 0 and `amplitude`.

All required keys of the chosen type are [E0005] when missing; keys of other
types are [E0004]. `seed` is an integer; the same seed gives the same
waveform. Values may be expressions of parameters.

Noise sources are parts like any other: they have pins, parameters and
tests, and can be placed in a circuit, or used inside a subcircuit as an
element (`part = "generic_noise_ripple"`).

## 13. Curves and CSV data files

### 13.1 Declaring a curve

```toml
[[curves]]
name = "current_vs_duty"                    # ^[a-z][a-z0-9_]*$
file = "data/generic_fan_4pin_current.csv"  # relative to this part file
x = "duty"                                  # column name in the file
y = "current"                               # column name in the file
x_unit = "%"
y_unit = "A"
interpolation = "linear"                    # linear, step, cubic (default linear)
extrapolation = "clamp"                     # clamp, linear, zero, error (default clamp)
description = "Supply current against PWM duty cycle"
```

- `file` is relative to the part file, uses `/`, and must stay inside the
  directory of the part file (no absolute paths, no `..`) [E0095]. It must
  exist [E0090].
- `x_unit` and `y_unit` must match the unit row of the file [E0031] and be
  known units [E0030]. `x` and `y` must be column names of the file [E0093].
- A curve can be used as a function in expressions: `current_vs_duty(duty)`,
  and by load branches with `mode = "curve"`.

### 13.2 Interpolation and extrapolation

| `interpolation` | Between two points |
|---|---|
| `linear` | straight line |
| `step` | value of the left point (zero-order hold) |
| `cubic` | monotone cubic (Fritsch-Carlson), no overshoot between points |

| `extrapolation` | Outside the x range |
|---|---|
| `clamp` | value at the nearest end point |
| `linear` | continue the first or last segment |
| `zero` | 0 |
| `error` | the simulation stops with an error |

### 13.3 CSV format

```
# SPDX-License-Identifier: CC-BY-4.0
# Source: author estimate for a typical 120 mm fan, see part file
duty,current,speed
%,A,rpm
0,0.03,600
50,0.09,1300
100,0.20,2200
```

- UTF-8, comma separated, `.` as decimal separator, no quotes.
- Lines starting with `#` are comments; empty lines are ignored. Start the
  file with `# SPDX-License-Identifier: CC-BY-4.0` and a line naming the
  source of the data.
- First non-comment line: column names (`^[a-z][a-z0-9_]*$`). Second: units,
  one per column, from the unit list (section 5.3).
- Then data rows: plain numbers (no prefixes, no units), the same number of
  cells as the header.
- The first column is the x axis; it must be strictly increasing [E0092].
  When a curve uses another column as `x`, that column must be strictly
  increasing too.
- At least two data rows.
- Any other format problem is [E0091], with line and column.

## 14. Reference tests

Every part should have at least one reference test [W0110]; quality
`datasheet` and `measured` require one [E0022]. Tests describe a small test
bench and the expected result. In format 0.1 the validator checks the
structure; the engine will run them in CI (roadmap stage 2 and 5).

```toml
[[tests]]
name = "forward_voltage"            # required, ^[a-z][a-z0-9_]*$
description = "Datasheet table 3"   # optional
analysis = "op"                     # op, dc, tran
params = { i_s = "1 nA" }           # instance parameter overrides [E0075, E0050]
features = { thermal = false }      # instance feature switches [E0060]

[[tests.stimulus]]
name = "i1"
nodes = ["a", "k"]
source = "current"                  # voltage or current
waveform = "dc"                     # dc, pulse, sin, pwl
value = "10 mA"

[[tests.elements]]                  # optional extra test bench elements, same keys as 9.3
name = "Rpull"
type = "R"
nodes = ["dq", "vdd"]
params = { r = "4.7 kOhm" }

[[tests.expect]]
probe = "V(a, k)"
value = "0.7 V"
abs_tol = "0.1 V"
```

- `analysis = "tran"` requires `stop` (value in s); `step` (maximum step) is
  optional. `analysis = "dc"` requires `sweep = { stimulus = "v1", from = 0,
  to = 5, step = 0.1 }`, where `stimulus` names a stimulus [E0113].
- Stimulus waveforms and their keys:
  `dc`: `value`;
  `pulse`: `v1`, `v2`, `delay`, `rise`, `fall`, `width`, `period`;
  `sin`: `offset`, `amplitude`, `frequency`, optional `delay`;
  `pwl`: `points = [[t0, y0], [t1, y1], ...]` (numbers in s and in V or A).
- Stimulus and test element nodes are pin names of the part, names of other
  test bench nodes (any `^[A-Za-z0-9_]+$` name used by `tests.elements`), or
  `0`. A pin not connected by any stimulus or test element is left open.
- An expectation has a `probe` expression (with `V()` and `I()`), and either
  `value` with optional `at` (time for `tran`, sweep value for `dc`), or
  `min`/`max` (range check over the whole analysis), or `file` and `column`
  (reference waveform CSV, x column = time or sweep value).
  Tolerance: `abs_tol` and/or `rel_tol` [E0111] (not needed for `min`/`max`).
  For a reference waveform both the value and the time axis are compared
  point by point after interpolation of the simulated result.
- `at` must lie inside the analysis range: `0 ≤ at ≤ stop` for `tran`,
  between `from` and `to` for `dc` [E0112].

## 15. Expressions

Expressions follow the syntax of SPICE B-source expressions as implemented
by common simulators, restricted to a subset that has the same meaning
everywhere.

### 15.1 Syntax

| Element | Syntax |
|---|---|
| Number | `12`, `1.5`, `2e-3`, `4.7k`, `10meg`; SPICE suffixes `f p n u m k meg g t`, case insensitive. Unit letters after a number (`1kOhm`) are not allowed [E0080]. |
| Identifier | parameter, signal, variable, input or built-in name |
| Operators (low to high precedence) | `c ? a : b`; `\|\|`; `&&`; `==` `!=`; `<` `<=` `>` `>=`; `+` `-`; `*` `/`; unary `-` `+` `!`; `^` or `**` (power, right associative) |
| Grouping | `( )` |
| Call | `name(arg, ...)` |

Logical operators return 1 or 0; any non-zero value counts as true.

In SPICE, `M` and `m` both mean milli, and only `meg` means mega. A number
followed by an upper-case `M` is accepted but warned about [W0083], because
it is a common mistake. Write `1meg` or `1e6`.

### 15.2 Built-in names

| Name | Meaning |
|---|---|
| `time` | simulation time in s |
| `temp` | circuit temperature in degC |
| `pi` | 3.14159... |
| `time_in_state` | state machines only: time since the current state was entered |

### 15.3 Functions

| Function | Arguments | Meaning |
|---|---|---|
| `abs sqrt exp ln log log10 sin cos tan asin acos atan sinh cosh tanh floor ceil round sgn` | 1 | as usual; `log` is the natural logarithm, as in SPICE |
| `u(x)` | 1 | unit step: 1 for x > 0, else 0 |
| `uramp(x)` | 1 | x for x > 0, else 0 |
| `atan2(y, x)`, `pow(x, y)`, `pwr(x, y)` | 2 | `pwr` is `sgn(x) * abs(x)^y` |
| `min(a, b, ...)`, `max(a, b, ...)` | 2 or more | |
| `limit(x, lo, hi)` | 3 | clamps x |
| `if(c, a, b)` | 3 | same as `c ? a : b` |
| `V(n)`, `V(n1, n2)` | node names | node voltage against ground, or difference |
| `I(x)` | pin or element name | current into pin `x` of this part, or through element `x` (voltage sources, inductors, `B` voltage sources, outputs with `source = "voltage"`) |
| `rising(x)`, `falling(x)` | input name | state machines, `on` only: edge events |
| `in_state(s)` | state name | state machines: 1 while in state `s` |
| curve name | 1 | value of the curve at x |

Unknown names are [E0081] (with suggestion); unknown functions or a wrong
number of arguments are [E0082]; any other syntax error is [E0080] with the
column of the problem. Node names inside `V()` and `I()` are checked like all
nodes [E0040].

## 16. Mapping to simulation elements

This section describes how the engine (roadmap stage 2 and 3) turns a part
into elements of its circuit matrix. It is informative: it explains intent,
the engine documentation will be the reference.

1. **Instantiation.** An instance gets the part's default parameters,
   overridden by instance values. Parameter expressions are evaluated once at
   the start; values that depend on `time`, `V()` or `I()` are evaluated at
   every step.
2. **Features.** Enabled features are the part defaults plus instance
   switches plus everything they `require`. Elements, outputs, branches and
   parameters of disabled features are removed, or shorted for
   `when_disabled = "short"`.
3. **Primitive.** One element of the engine's built-in set (R, C, L, diode,
   BJT and MOSFET level 1 with SPICE-compatible parameters, switch,
   independent and controlled sources).
4. **Subcircuit.** Flattened into the parent with hierarchical names
   (`X1.R2`, nodes `X1.n1`); `0` stays global ground. Part instances inside
   it are expanded recursively.
5. **Behavioral outputs and `B` elements.** Nonlinear controlled sources whose
   value is the expression; derivatives for the Newton iteration are formed
   automatically. Voltage outputs with `r_out` get a series resistor.
   Switch outputs become a resistor that changes between `ron` and `roff`.
6. **Signals.** Auxiliary unknowns of the circuit; with `tau` a first-order
   filter state is added.
7. **Load branches.** A controlled current source between the branch nodes.
   `quantity = "resistance"` becomes `I = V / R`; `quantity = "power"`
   becomes `I = P / V` above `v_min` and a resistor below.
   `mode = "curve"` uses a table lookup with the declared interpolation.
8. **State machines.** An event-driven process outside the matrix. Threshold
   crossings of inputs and `after` timeouts become time step breakpoints, so
   edges are located exactly. When the state changes, the values of branches
   and outputs change at that breakpoint; the analog solution then continues.
9. **Noise sources.** A time series generated before the run from the seed
   and parameters, applied as an independent source with breakpoints.
10. **Tests.** Each `[[tests]]` entry is turned into a small netlist: the
    part instance, stimulus sources, test elements, and the requested
    analysis; the probes are compared with the expectation and tolerance.

## 17. Using a part in a circuit

Preview of the instance syntax used by netlists and sidecar files
(`<project>.mspice.toml`). The final form is defined with the engine
(stage 3 and 4).

```toml
[[instances]]
name = "C12"
part = "generic_capacitor"
nodes = { p = "VCC", n = "GND" }
params = { capacitance = "10 uF" }
features = { esr = true }
```

## 18. Versioning of this format

- `format_version` is `MAJOR.MINOR`. A validator accepts files of its own
  major version with a minor version up to its own.
- While the major version is 0, any minor version may change keys; the
  validator and this document change together, and `CHANGELOG` entries in
  this file list the changes.

### Changelog

- 0.1 (2026-11-27): first version.
