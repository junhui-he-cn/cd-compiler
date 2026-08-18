# Half-Open Range Operator

Status: accepted and implemented on 2026-08-18.

## Decision

The source language accepts the contiguous `start..<stop` operator. It creates
the same immutable half-open integer range as `range(start, stop)` and always
uses step `1`. Both operands are evaluated once from left to right. The
operator is parsed at comparison precedence, with arithmetic expressions kept
inside each endpoint through the existing `term` grammar.

The operator is intrinsic and does not resolve a user binding named `range`.
The existing shadowable `range(...)` function remains available for one-, two-,
and three-argument forms and custom steps. Closed ranges (`..=`) and slicing
are out of scope.

## Compatibility

No new IR or bytecode opcode is introduced. C++ lowering emits the existing
`range` native call with two registers, so the `cdbc 0.2` artifact contract,
Rust VM behavior, O0 default, interpreter-default execution, and C++/Rust
parity remain unchanged.

## Verification

`tests/golden/range_operator` covers AST, IR, bytecode, formatter-compatible
syntax, indexing, length, and array/range iteration. Parse and type fixtures
cover a missing stop operand and a non-numeric start operand. The formatter
unit test verifies canonical `..<` spelling and AST preservation.
