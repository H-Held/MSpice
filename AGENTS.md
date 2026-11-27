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
