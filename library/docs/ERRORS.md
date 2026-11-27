# Diagnostic codes of mspice-libcheck

Every diagnostic has a stable code. Codes starting with `E` are errors (the
file must be fixed; exit code 1), codes starting with `W` are warnings (exit
code 0 unless `--deny-warnings` is given). Codes are never reused for another
meaning; a retired code stays documented.

Long explanations with a wrong and a corrected example are in
[errors/](errors/) and are built into the tool:
`mspice-libcheck explain E0004`. `mspice-libcheck codes --format=json` lists
all codes for programs.

| Code | Severity | Meaning |
|---|---|---|
| [E0001](errors/E0001.md) | error | TOML syntax error |
| [E0002](errors/E0002.md) | error | file cannot be read |
| [E0003](errors/E0003.md) | error | missing or unsupported format_version |
| [E0004](errors/E0004.md) | error | unknown or misplaced key |
| [E0005](errors/E0005.md) | error | missing required key |
| [E0006](errors/E0006.md) | error | wrong value type |
| [E0007](errors/E0007.md) | error | value is not one of the allowed words |
| [E0010](errors/E0010.md) | error | invalid part id |
| [E0011](errors/E0011.md) | error | duplicate part id |
| [E0012](errors/E0012.md) | error | invalid name |
| [E0013](errors/E0013.md) | error | duplicate name |
| [E0014](errors/E0014.md) | error | invalid version or version requirement |
| [E0020](errors/E0020.md) | error | incomplete source |
| [E0021](errors/E0021.md) | error | licence not accepted |
| [E0022](errors/E0022.md) | error | quality level requirements not met |
| [E0030](errors/E0030.md) | error | unknown unit |
| [E0031](errors/E0031.md) | error | unit mismatch |
| [E0032](errors/E0032.md) | error | malformed quantity |
| [E0033](errors/E0033.md) | error | value out of range |
| [E0034](errors/E0034.md) | error | minimum greater than maximum |
| [E0035](errors/E0035.md) | error | invalid tolerance |
| [E0040](errors/E0040.md) | error | unknown pin or node |
| [W0041](errors/W0041.md) | warning | pin not used by the model |
| [E0042](errors/E0042.md) | error | wrong number of nodes or unconnected pin |
| [E0050](errors/E0050.md) | error | parameter is not adjustable |
| [E0060](errors/E0060.md) | error | unknown feature |
| [E0061](errors/E0061.md) | error | feature dependency cycle |
| [E0070](errors/E0070.md) | error | unknown element parameter |
| [E0071](errors/E0071.md) | error | missing element parameter |
| [E0072](errors/E0072.md) | error | conflicting or missing alternative keys |
| [E0073](errors/E0073.md) | error | unknown part |
| [E0074](errors/E0074.md) | error | part contains itself |
| [E0075](errors/E0075.md) | error | unknown instance parameter |
| [E0080](errors/E0080.md) | error | expression syntax error |
| [E0081](errors/E0081.md) | error | unknown name in expression |
| [E0082](errors/E0082.md) | error | unknown function or wrong argument count |
| [W0083](errors/W0083.md) | warning | ambiguous SPICE suffix M |
| [E0090](errors/E0090.md) | error | data file not found |
| [E0091](errors/E0091.md) | error | malformed CSV file |
| [E0092](errors/E0092.md) | error | x axis not strictly increasing |
| [E0093](errors/E0093.md) | error | CSV column not found |
| [E0094](errors/E0094.md) | error | unknown curve |
| [E0095](errors/E0095.md) | error | data file path not allowed |
| [E0100](errors/E0100.md) | error | unknown state |
| [E0101](errors/E0101.md) | error | unreachable state |
| [W0102](errors/W0102.md) | warning | state without way out |
| [E0103](errors/E0103.md) | error | unknown branch in state values |
| [W0110](errors/W0110.md) | warning | part has no reference tests |
| [E0111](errors/E0111.md) | error | expectation without tolerance |
| [E0112](errors/E0112.md) | error | expectation outside the analysis range |
| [E0113](errors/E0113.md) | error | unknown stimulus |
| [W0120](errors/W0120.md) | warning | pattern matches no file |
| [E0121](errors/E0121.md) | error | dependency not found |
| [E0122](errors/E0122.md) | error | dependency version not satisfied |
| [E0123](errors/E0123.md) | error | merge conflict |
| [W0124](errors/W0124.md) | warning | identical part defined twice |
| [E0125](errors/E0125.md) | error | invalid override |
| [E0126](errors/E0126.md) | error | dependency cycle |

Code ranges: E0001-E0007 file and key structure; E0010-E0014 identity and
versions; E0020-E0022 provenance and licence; E0030-E0035 units and values;
E0040-E0042 pins and nodes; E0050 parameters; E0060-E0061 features;
E0070-E0075 elements and part references; E0080-W0083 expressions;
E0090-E0095 data files; E0100-E0103 state machines; W0110-E0113 reference
tests; W0120-E0126 libraries and merging.

## Adding a code

1. Add it to the table in `tools/libcheck/src/diag.cpp` (next free number in
   its range).
2. Write `docs/errors/<CODE>.md` with the sections `## Wrong` and `## Correct`
   (the `selftest` CTest fails otherwise).
3. Add a negative test `tests/invalid/<CODE>_<what>.toml` with a matching
   `.expected` file (the `every_code_has_negative_test` CTest fails otherwise).
4. Add the row to this table and mention the code in SPEC.md.
