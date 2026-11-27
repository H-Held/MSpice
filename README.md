# MSpice

MSpice is a circuit simulator for transient analysis of board-level
electronics. It runs from the command line on Windows and Linux and comes
with an open part library format that people and language models can extend.

> Status: early. The engine was taken over from an internal research project
> in C++ and is being rewritten in Rust ([plan](docs/RUST_MIGRATION.md)).
> Interfaces and file formats will change before version 1.0.

## What is in this repository

| Folder | Content |
|---|---|
| [engine/](engine/) | Simulator core and the `mspice` command line tool |
| [library/](library/) | Part library format, validator, generic parts, load models, guides |
| [studio/](studio/) | Graphical front end (planned) |
| [docs/](docs/) | Design notes and the [roadmap](docs/ROADMAP.md) |

## Building

Current C++ reference build. Requirements: CMake 3.21 or newer, Ninja, a C++20 compiler (GCC 12+, Clang 15+
or MSVC 2022).

```sh
cmake -S engine -B build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine
ctest --test-dir build/engine --output-on-failure
```

Details, including Windows with MSYS2, are in [engine/README.md](engine/README.md).

## Contributing

Bug reports, ideas and pull requests are welcome. Please read
[CONTRIBUTING.md](CONTRIBUTING.md) first; every commit needs a
`Signed-off-by` line (`git commit -s`).

New parts are especially welcome. The library guide explains how to describe a
part from its datasheet and how to check it before you open a pull request.

## Licence

The code is licensed under the Apache License 2.0 with the Commons Clause
condition: you may use, modify and redistribute it, also inside a company, but
you may not sell MSpice itself or a service whose value comes substantially
from it. See [LICENSE](LICENSE).

Part models in `library/parts` are licensed under CC BY 4.0 unless the file
states otherwise.

The software is provided "as is", without warranty of any kind. The authors
accept no liability for damage resulting from its use, including damage to
hardware built from simulation results.

MSpice is not affiliated with Cadence (PSpice), Synopsys (HSPICE) or Analog
Devices (LTspice). Product names are trademarks of their respective owners
and are used only to describe compatibility.
