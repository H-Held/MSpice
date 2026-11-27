# Creating a part

This guide walks through writing a part file by hand. The full rules are in
[SPEC.md](../SPEC.md); language models should follow
[AGENT_GUIDE.md](AGENT_GUIDE.md), which has the same steps in a stricter form.

## Before you start

- Build the validator once (from the repository root):

  ```sh
  cmake -S library/tools/libcheck -B build/libcheck -G Ninja
  cmake --build build/libcheck
  ```

  The program is `build/libcheck/mspice-libcheck` (`.exe` on Windows).
- Pick the closest existing part in `library/parts/` as a starting point.
  `parts/loads/generic_load_custom.toml` is a template for consumers.
- Have the source at hand: a datasheet (note title, vendor, revision and the
  table or figure you use), your own measurement, or an openly licensed SPICE
  model. Write values down as numbers; never copy datasheet text, figures or
  vendor SPICE files into the repository.

## Step 1: identity and source

```toml
# SPDX-License-Identifier: CC-BY-4.0
format_version = "0.1"

[part]
id = "generic_temp_sensor_i2c"      # generic_<function>, lower case
name = "I2C temperature sensor (supply model)"
version = "0.1.0"
category = "load/sensor"
description = "What the model covers, and what it leaves out."
compatible = ["XYZ123"]             # real part numbers only here
author = "Your Name"
license = "CC-BY-4.0"
quality = "draft"

[part.source]
kind = "datasheet"
title = "XYZ123 digital temperature sensor"   # title as printed on the datasheet
vendor = "Example Semiconductor"
revision = "Rev. C, 2025-03"                    # as printed on the datasheet
section = "Table 5, electrical characteristics"
```

Start with `quality = "draft"`. Raise it to `datasheet` only when every value
is traceable to the named revision and section and the part has reference
tests.

## Step 2: pins

One `[[pins]]` entry per terminal, in the order of the physical pins or of
the usual symbol. Roles: `power`, `ground`, `input`, `output`,
`bidirectional`, `analog`. Add absolute maximum ratings if you know them:

```toml
[[pins]]
name = "vdd"
role = "power"
reference = "gnd"
abs_max_voltage = "4 V"
```

## Step 3: parameters

Everything a user might want to change, and every number the model uses,
becomes a parameter with a unit:

```toml
[[parameters]]
name = "i_active"
type = "real"
unit = "A"
default = "10 uA"
min = 0
description = "Supply current during conversion (typical)"
```

Values can be numbers, quantity strings (`"4.7 kOhm"`, `"10 uA"`) or
expressions in braces (`"{2 * i_active}"`). In quantity strings `M` is mega
and `m` is milli. Mark values that describe the part itself and must not
change per instance with `adjustable = false`. Add `tolerance` where the
datasheet gives a spread.

## Step 4: the model

Choose the simplest kind that describes the behaviour (SPEC.md section 9):

| You have | Use |
|---|---|
| One R, C, L, diode, transistor, switch | `primitive` |
| A small equivalent circuit | `subcircuit` |
| An output that follows a formula | `behavioral` |
| A consumer: current depends on time, a curve, a protocol | `load` |
| An interference generator | `noise_source` |

Optional behaviour (parasitics, inrush, temperature effects) goes behind a
feature, so users can switch it off:

```toml
[[features]]
name = "esr"
description = "Equivalent series resistance"

[[model.elements]]
name = "Resr"
type = "R"
nodes = ["n1", "n2"]
params = { r = "{esr}" }
feature = "esr"
when_disabled = "short"
```

Curves from a datasheet figure go into a CSV file next to the part
(`data/<part>.csv`): read the points off the figure and write them as
numbers, with a comment naming the figure. Do not include the figure.

## Step 5: reference tests

Add at least one `[[tests]]` entry: a small test bench (stimulus sources and
extra elements) and expected values with a tolerance. Take expected values
from the datasheet (typical characteristics) or calculate them by hand, and
write the calculation into `description`. These tests will run in CI once
the engine executes them.

## Step 6: validate

```sh
build/libcheck/mspice-libcheck check library/
```

Fix every error. Each message names a code; `mspice-libcheck explain E0031`
prints a wrong and a corrected example. Warnings are allowed locally but CI
runs with `--deny-warnings`.

## Step 7: pull request

- Put the file into `library/parts/` (or `parts/loads/`, `parts/noise/`).
- Commit with sign-off: `git commit -s -m "library: add generic_temp_sensor_i2c"`.
- In the pull request, say where the values come from and what you did not
  model.
