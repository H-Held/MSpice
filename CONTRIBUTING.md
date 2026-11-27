# Contributing to MSpice

Thanks for taking the time. Issues, ideas and pull requests are all welcome.

## Before you start

- For larger changes, open an issue first so we can agree on the approach.
- Read the [design notes](docs/design/2026-11-27-mspice-design.md) and the
  [roadmap](docs/ROADMAP.md).

## Rules for every change

1. **Tests.** Every feature or fix comes with a positive test (valid input,
   expected result) and a negative test (invalid input, exact expected error).
2. **No foreign material we may not redistribute.** Do not add vendor SPICE
   models, copied datasheet text or figures, or code under GPL or other
   copyleft licences. If you are unsure, ask in the issue.
3. **Generic names.** Parts are named `generic_<function>` (for example
   `generic_rgb_led_addressable`). Real part numbers go only into the
   `compatible` field.
4. **English** for code, comments, documentation and commit messages.
5. **Commit messages** follow `area: short summary` (for example
   `engine: add tolerance sweep`), imperative mood, at most 72 characters in
   the first line.

## Developer Certificate of Origin (DCO)

Every commit must be signed off:

```sh
git commit -s -m "library: add generic_fan_4pin"
```

This adds a line `Signed-off-by: Your Name <you@example.com>`. With it you
confirm the [Developer Certificate of Origin 1.1](https://developercertificate.org/):
that you wrote the change or have the right to submit it under the project's
licence. Pull requests without sign-off cannot be merged.

## Contributing part models as a company

You keep the credit: the `author` and `source` fields of a part name you and
the datasheet. Models are published under CC BY 4.0. A model can be marked as
vendor-verified once signatures are available (roadmap stage 5).

## Reporting security issues

See [SECURITY.md](SECURITY.md).
