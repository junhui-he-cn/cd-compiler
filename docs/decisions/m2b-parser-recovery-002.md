# M2B-PARSE-002: recover malformed methods inside `impl` blocks

Status: resolved and implemented.

## Decision

An `impl` declaration is a parser recovery boundary for its method list:

- each method is attempted independently and contributes at most one Parse
  diagnostic when its header or body cannot be parsed;
- recovery skips malformed method content with brace awareness, resumes at the
  next top-level `fun` method or the closing `}` of the `impl`, and never
  consumes that outer closing brace; and
- parser-local block depth and contextual constructor eligibility are restored
  before the next method is attempted.

The recovered method is omitted from the partial AST, while later valid methods
remain available only for recovery progress; any recorded Parse diagnostic still
prevents semantic and backend stages. Existing declaration/block recovery,
diagnostic order, direct-input stop-first behavior, and accepted syntax are
unchanged.

## Evidence and gate

`m2b_parser_recovery_impl` contains a malformed method header with a nested block,
followed by a valid method and a valid top-level statement. The expected result
is one parameter diagnostic. The malformed corpus binds the same nested-brace
resynchronization case to a stable case ID.

Focused verification:

```sh
cmake --build build --target compiler_design
python3 tests/run_golden_tests.py ./build/compiler_design --case m2b_parser_recovery_impl
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs
python3 tests/verification_inventory.py --write
git diff --check
```

## Deletion boundary

There is no duplicate method parser to delete. Recovery for enum/struct field
lists, match-arm lists, and direct/per-module semantic recovery remains outside
this slice and requires its own compatibility cases.
