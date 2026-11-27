# Guide for AI agents: creating and fixing parts

This guide is written for language models and other automated agents that
create or repair MSpice part files. Follow the steps in order. The format is
defined in [SPEC.md](../SPEC.md); the JSON Schemas in [../schema/](../schema/)
describe the same data model for tools.

## Rules that are never negotiable

1. Do not copy vendor SPICE models, datasheet text or datasheet figures into
   the repository. Transfer numbers (facts) only and cite where they come
   from.
2. Part ids are `generic_<function>` (`^generic_[a-z0-9_]+$`). Real part
   numbers go only into `compatible`.
3. Never invent precision. If a value is an estimate, say so in
   `description` or `source.notes`, and keep `quality = "draft"`.
4. `license = "CC-BY-4.0"` (or `CC0-1.0`) for every part.
5. Do not change files outside the part you are working on unless asked.

## Task A: from a datasheet to a part

1. **Collect facts.** From the datasheet note: title, vendor, document number,
   revision, and for every value the table or figure it comes from. Prefer
   typical values for the model and min/max for `tolerance` and pin limits.
2. **Choose the model kind** (SPEC.md section 9). Prefer the simplest kind
   that reproduces the behaviour a circuit designer needs. Write down what is
   not modelled.
3. **Write the file** in this key order: `format_version`, `[part]`,
   `[part.source]`, `[[pins]]`, `[[parameters]]`, `[[features]]`,
   `[[curves]]`, `[model]` with its sub-tables, `[[tests]]`. Start from the
   closest file in `library/parts/`.
4. **Units.** Every real or integer parameter has a `unit` from SPEC.md
   section 5.3. Quantity strings use SI prefixes (`M` = mega, `m` = milli);
   expressions use SPICE suffixes (`meg` = mega, `m` = milli).
5. **Curves.** Digitise figure points into `data/<id>.csv` (comment lines,
   column names, unit line, numbers). Add a comment line naming the figure.
6. **Tests.** Add at least one `[[tests]]` per important datasheet
   characteristic, with the expected value, a tolerance, and the source in
   `description`.
7. **Quality.** Use `datasheet` only if every value is traceable to one
   revision and section, `source.vendor`, `source.revision` and
   `source.section` are set and tests exist. Otherwise `draft`.
8. **Validate** (see "Validate-fix loop").

## Task B: from an openly licensed SPICE model to a part

1. **Licence check first.** Find the licence text of the model file or its
   repository. Continue only if it is one of: `CC0-1.0`, `CC-BY-4.0`, `MIT`,
   `BSD-2-Clause`, `BSD-3-Clause`, `Apache-2.0`, `Zlib`. If the file has
   vendor terms ("for use with ... only", "confidential", "may not be
   redistributed"), an encryption block, or no licence at all: stop and
   report that the model cannot be used. Never guess a licence.
2. Record it:

   ```toml
   [part.source]
   kind = "spice_model"
   title = "<model name and file name>"
   vendor = "<publisher of the model>"
   model_license = "MIT"
   url = "<where the model was found>"
   ```

3. **Map the model.** `.model` cards of D, Q, M (level 1) and SW map to
   `primitive` with the parameter names of SPEC.md section 9.2 (lower case).
   `.subckt` definitions map to `subcircuit`; `B` sources map to `B` elements
   with the same expression (check it against SPEC.md section 15; replace
   unsupported functions and report them). Every `.param` becomes a
   `[[parameters]]` entry with a unit.
4. **Report what was dropped.** Anything you could not map (unsupported
   element, level 3 MOSFET parameters, `table()` functions) must be listed in
   `description`. Never drop it silently.
5. **Tests.** Simulate the original model if a simulator is available, or use
   datasheet curves, and add the results as `[[tests]]`.

## Validate-fix loop

Run the validator with JSON output:

```sh
mspice-libcheck check --format=json library/
```

Output (schema `mspice-libcheck-diagnostics/1`):

```json
{
  "schema": "mspice-libcheck-diagnostics/1",
  "diagnostics": [
    {"code": "E0081", "severity": "error",
     "message": "unknown name `resistence` in `r` in [model] (`R`, resistor)",
     "file": "library/parts/generic_x.toml", "line": 36, "column": 18, "length": 10,
     "hint": "did you mean `resistance`?",
     "explain": "mspice-libcheck explain E0081"}
  ],
  "summary": {"files": 25, "errors": 1, "warnings": 0}
}
```

Loop:

1. Run the command. Exit code 0 and `errors == 0`: done (CI also requires
   `warnings == 0`, use `--deny-warnings`).
2. Take the first diagnostic. Open `file` at `line`/`column`.
3. Apply the `hint` if it is specific ("did you mean ..."). Otherwise run
   `mspice-libcheck explain <code>`, compare with its "Wrong" and "Correct"
   examples, and fix.
4. Fix one diagnostic at a time where diagnostics may depend on each other
   (for example a renamed parameter), then run again. Independent
   diagnostics in different files can be fixed together.
5. Stop after 10 rounds without progress and report the remaining
   diagnostics instead of guessing.

Do not silence a diagnostic by deleting content that carries meaning
(tests, sources, pins). Do not lower tolerances or invent values to make
tests pass.

Exit codes: 0 no errors, 1 errors (or warnings with `--deny-warnings`),
2 usage error, 3 output could not be written. `mspice-libcheck codes
--format=json` lists every code.

## Checklist before handing over

- [ ] `id` matches `generic_[a-z0-9_]+`; real part numbers only in `compatible`.
- [ ] `[part.source]` names the document (title, vendor, revision, section) or
      says `estimate`/`textbook`; SPICE-derived parts have an open
      `model_license`.
- [ ] `quality` is `draft` unless all requirements of SPEC.md section 4.4 hold.
- [ ] Every real/integer parameter has a unit; limits and tolerances are set
      where the datasheet gives them.
- [ ] Every pin is used by the model.
- [ ] `description` says what is not modelled.
- [ ] Data files are CSV next to the part, with a source comment, numbers only.
- [ ] At least one `[[tests]]` entry with tolerance; expected values are
      explained.
- [ ] `mspice-libcheck check --deny-warnings library/` passes.
- [ ] No vendor text, figure or model file was added.
