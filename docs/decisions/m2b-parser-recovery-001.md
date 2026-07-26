# M2B-PARSE-001: state-safe parser boundary recovery

Status: resolved and implemented.

## Decision

Parser recovery remains a syntax-stage service at declaration and block
statement boundaries:

- a `ParseError` is recorded at the first invalid token for the current
  declaration or statement;
- synchronization advances to a semicolon, a block boundary, or the next
  statement starter and then resumes parsing independent input;
- parser state that is local to the failed boundary, including block depth and
  contextual struct-constructor eligibility, is restored before synchronization;
  and
- diagnostics retain source order, source snippets, caret placement, exit status
  `1`, and empty stdout. No partial AST is sent to semantic or backend stages.

This slice does not recover from lexer errors, continue after a malformed
expression inside one statement, or alter direct-input/type-checking stop-first
rules. The existing statement-boundary recovery remains the compatibility
model; this change makes its state boundary explicit and exception-safe.

## Evidence and gate

`m2b_parser_recovery_state` begins with a malformed condition and then contains
a valid struct declaration, constructor expression, and field access. The
expected result is exactly the condition diagnostic: a failed condition must
not leave constructor parsing disabled for later statements.

The malformed corpus continues to cover multiple independent parser errors,
and the complete parse-error golden family remains in the verification
inventory.

Focused verification:

```sh
cmake --build build --target compiler_design
python3 tests/run_golden_tests.py ./build/compiler_design --case m2b_parser_recovery_state
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs
python3 tests/verification_inventory.py --write
git diff --check
```

## Deletion boundary

There is no duplicate parser implementation to delete in this slice. A later
M2B decision is still required before changing lexer stop-first behavior,
per-statement recovery limits, or the direct-input semantic boundary.
