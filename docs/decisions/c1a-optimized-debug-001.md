# C1A-OPT-DEBUG-001: optimized source observability contract

Status: implemented on 2026-08-03 on the explicit O1 pipeline.

## Decision

An O1 artifact keeps source-backed debugger observability while allowing the
optimizer to remove, reorder, or merge ordinary instructions. The contract is:

- every executed source-backed O1 instruction has a valid line/column location
  and source-local range;
- a de-SSA copy or control-flow barrier generated from an original instruction
  inherits that instruction's `SourceSpan` when the span is available;
- O0 and O1 preserve the same semantic trace sequence for function entry,
  output, return, runtime error, and function exit, including their function
  identities and displayed values;
- O1 may emit fewer or differently ordered `line` events and different physical
  instruction numbers, because those events describe the optimized stream;
- source line and range breakpoints continue to resolve in O1, and runtime-cell
  locals/captured values remain visible at debugger pauses; and
- metadata-free artifacts retain the existing explicit unknown-location
  behavior.

The source-span provenance table is supplied from the original IR stream to
de-SSA. This matters when O1 removes the original instruction before creating
an edge copy whose anchor still refers to that source instruction.

## Compatibility and migration

This is an internal compiler/debugger contract. It does not change the
`cdbc 0.1` sections, bytecode opcodes, Rust VM execution rules, O0 default, or
virtual-register representation. It does not promote runtime-cell bindings,
perform physical register allocation, or make O1 the default.

Existing metadata-free and source-backed O0 artifacts remain valid. The
current line/column and range metadata remain the shared artifact boundary;
no optimizer-specific metadata is serialized.

## Quantitative gate

`tests/debugger_tests.py` covers O0/O1 trace and debugger parity for:

- source locations, frame locals, stepping, and range breakpoints;
- nested branches and loops;
- nested closures and captured locals;
- imported functions after module-product linking;
- eliminated values and optimized control flow; and
- ordinary runtime failures and their call-stack diagnostics.

The focused gate includes the SSA/optimizer CTest cases, the debugger CTest,
Rust VM trace/debug execution, and the existing optimizer CLI/cache checks.

## Boundary

C1A does not decide the O1 default level, virtual versus physical register
policy, optimized local materialization beyond the current runtime-cell
contract, or any O2 pass. Those remain C1B/C1C decisions.

## Old-path deletion condition

Keep O0 as the compatibility default and retain the current metadata-free
fallback until C1B/C1C evidence covers register pressure, debug materialization,
artifact size, compile time, cache identity, runtime behavior, and default-level
migration.
