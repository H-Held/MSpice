# Rules for AI agents working in this repository

Read this file, [README.md](README.md), [docs/design/2026-11-27-mspice-design.md](docs/design/2026-11-27-mspice-design.md),
[docs/ROADMAP.md](docs/ROADMAP.md) and [docs/TASKS.md](docs/TASKS.md) before changing anything.

## Branches

| Branch | Purpose | Who merges |
|---|---|---|
| `main` | Released, stable state | Maintainers only |
| `dev` | Integration of finished work | Maintainers, through pull requests |
| `rust` | Rewrite of engine and tools in Rust | Merged into `dev` once parity is reached |
| `task/<ID>-<slug>` | One task from TASKS.md | Pull request into `dev` (or `rust` during the rewrite) |

Never push to `main` or `dev` directly. Never force-push shared branches.

## Every change

- Positive and negative tests for every feature or fix. Negative tests assert
  the exact error code and message.
- Never loosen a test tolerance or acceptance threshold to make a test pass.
  If a threshold looks wrong, stop and document it.
- No material we may not redistribute: no vendor SPICE models, no copied
  datasheet text or figures, no GPL/LGPL/AGPL code or dependencies (LGPL only
  as optional, dynamically linked, documented in THIRD_PARTY.md).
- Part ids are `generic_*`; real part numbers only in `compatible`.
- English for code, comments, docs and commit messages.
- Commits: `area: summary` (areas: engine, library, studio, docs, ci, repo),
  signed off with `git commit -s`.
- Update the status of the task in docs/TASKS.md in the same pull request.

## Handoff

Long-running work keeps a handoff note in `handoff/<topic>.md` (not committed,
see .gitignore): state, what was verified (exact command and result), open
problems, next step. Update it after every finished step so another agent can
continue after an interruption.

## Verification before claiming success

Run the full test suite of every component you touched and quote the summary
line. State what was not verified (for example: Linux build only in CI).

## Rust (branch `rust`, later all branches)

The project is moving from C++ to Rust ([docs/RUST_MIGRATION.md](docs/RUST_MIGRATION.md)).
Until phase R4, `engine/` (C++) is the reference implementation: change it
only to fix bugs that affect the golden data, never to add features.

Workspace layout:

| Crate | Content |
|---|---|
| `mspice-core` | circuit, MNA, solvers, devices, transient engine |
| `mspice-spice` | SPICE lexer, parser, expressions, import |
| `mspice-cli` | binary `mspice` |
| `mspice-libformat` | library data model, TOML loading, validation |
| `mspice-libcheck` | binary `mspice-libcheck` |

Rules:

- Before every commit: `cargo fmt --check`, `cargo clippy --all-targets --
  -D warnings`, `cargo test --workspace`, `cargo deny check`.
- No `unsafe` outside clearly marked modules (solver kernels, C ABI); each
  `unsafe` block has a `// SAFETY:` comment.
- No panics on user input: parsers return errors with the stable diagnostic
  codes (`E0001`...), never `unwrap()` on data from files or the network.
- Errors carry file, line and column; every code has an `explain` text and a
  negative test.
- Numerics: no change of floating-point evaluation order in hot paths without
  comparing against the golden data; tolerances come from the original tests.
- Performance claims need a benchmark (median of at least 5 runs, release
  build, same machine as the reference) recorded in the pull request.
- Public API: documented with rustdoc examples that run as doc tests; the C
  ABI and JSON-RPC API follow [docs/VERSIONING.md](docs/VERSIONING.md).
- Dependencies: only licences allowed in `deny.toml`; each new crate is added
  to THIRD_PARTY.md with its licence.
