# M2B-PARSE-003: recover declaration member lists

Status: resolved and implemented.

## Decision

Struct fields and enum variants are recovered within their declaration member
lists:

- each malformed member contributes at most one Parse diagnostic;
- synchronization stops at a top-level comma or the declaration's closing
  `}` while ignoring nested parameter, array, and generic-type delimiters;
- a following valid member remains parseable after a malformed member; and
- existing delimiter policy is preserved, including the existing struct
  trailing-comma rejection and enum trailing-comma acceptance.

The recovered member is omitted from the partial AST, but any recorded Parse
diagnostic still prevents semantic and backend stages. Method-list recovery,
expression-internal recovery, lexer behavior, and direct-input/type stop-first
behavior are unchanged.

## Evidence and gate

`m2b_parser_recovery_members` contains one malformed struct field and one
malformed enum variant, each followed by valid members. The expected result is
the two source-ordered member diagnostics and no stdout. The malformed corpus
binds the same declaration-list case to a stable case ID.

Focused verification:

```sh
cmake --build build --target compiler_design
python3 tests/run_golden_tests.py ./build/compiler_design --case m2b_parser_recovery_members
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs
python3 tests/verification_inventory.py --write
git diff --check
```

## Deletion boundary

Expression-internal lists and match-arm recovery remain separate parser
boundaries. No legacy parser implementation is deleted by this slice.
