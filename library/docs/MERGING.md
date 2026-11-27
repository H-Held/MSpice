# Using and merging several libraries

A company or a project can keep its own parts in a separate library and use
it together with the generic library. The rules are in
[SPEC.md](../SPEC.md) sections 2 and 3; this page shows the usual setups.

## A library that builds on the generic one

```
acme-parts/
  library.toml
  parts/generic_temp_sensor_1wire.toml   # measured version
  parts/generic_acme_valve.toml
```

```toml
# acme-parts/library.toml
# SPDX-License-Identifier: CC-BY-4.0
format_version = "0.1"

[library]
name = "acme-parts"
version = "1.0.0"
description = "Parts measured at ACME"
license = "CC-BY-4.0"
parts = ["parts/**/*.toml"]

[[dependencies]]
name = "mspice-generic"
path = "../MSpice/library"
version = ">=0.1.0, <0.2.0"

[[overrides]]
id = "generic_temp_sensor_1wire"
library = "mspice-generic"
reason = "Conversion current measured on 20 samples"
```

`mspice-libcheck check acme-parts/` loads the dependency, checks both
libraries and resolves references across them.

## Rules in short

| Situation | Result |
|---|---|
| Same id in two libraries, override declared by the later one | the later part is used |
| Same id, identical files | used once, warning W0124 |
| Same id, different files, no override | error E0123 |
| Override names an id the other library does not have | error E0125 |
| Same id twice inside one library | error E0011 |
| Dependency missing or wrong name | error E0121 |
| Dependency version outside the requirement | error E0122 |
| Libraries depend on each other in a circle | error E0126 |

"Later" means: dependencies come first, then the library that depends on
them.

## Merging into one directory

```sh
mspice-libcheck merge -o merged/ MSpice/library acme-parts
```

- Libraries are taken in command line order; each later one is treated as
  depending on the earlier ones, so its `[[overrides]]` apply.
- The output contains `library.toml`, one subdirectory per source library
  with the kept part files and their data files (copied byte for byte), and
  `MERGE_REPORT.json`, which lists every part and every replaced part with
  the reason.
- Without overrides, conflicts stop the merge (nothing is written). For a
  one-off merge, `--on-conflict=first` keeps the earlier part and
  `--on-conflict=last` the later one; the choice is recorded in the report.
- `--force` allows writing into a directory that already has a
  `library.toml`.

Check the result with `mspice-libcheck check merged/`.

## Licences

Part files carry their own `license`. The merged manifest takes the licence
of the first library if all agree, otherwise `CC-BY-4.0`; the report lists
the licence of every source library. Attribution requirements of CC BY 4.0
are met by keeping `author` and `source` in every part file, which the merge
never changes.
