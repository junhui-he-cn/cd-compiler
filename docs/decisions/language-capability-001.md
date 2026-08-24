# Language 0.2 Capability Constraints

Status: decided on 2026-08-24.

## Decision

`Eq`, `Ord`, and `Hash` remain built-in compile-time generic constraints.
Constraint sets such as `Eq + Hash` remain valid. Concrete types satisfy these
constraints through the existing compiler rules, and type parameters inherit
their declared constraints.

`Ord` is satisfied by `number` and `string` and implies `Eq`. `Eq` and `Hash`
continue to use the existing primitive, nullable, collection, and identity
rules. Named structs do not satisfy `Ord`; struct ordering operators were
removed and are not reopened by this decision.

Capabilities are erased after front-end type checking. Capability names do not
reach IR, bytecode, module artifacts, or the Rust VM.

## Rationale

The compiler already enforces capability requirements at generic calls,
operators, native `hash`, callbacks, public interfaces, and `.cdi` sidecars.
Keeping the model compile-time-only preserves the current C++/Rust parity and
avoids introducing runtime witness dictionaries, dynamic dispatch, or a new
artifact contract before those boundaries are designed.

## Compatibility

This decision adds no source syntax, IR operation, bytecode opcode, artifact
version, or VM behavior. User-defined capability declarations, capability
`impl` blocks, trait objects, witness dictionaries, and automatic custom
struct equality/order remain out of scope. Reopening that direction requires a
separate decision covering syntax, module interfaces, lowering, artifacts, and
runtime behavior.

## Verification

The existing `type_utils` tests and generic capability positive/negative
fixtures validate the contract. The full current verification run also passed
CTest 47/47, golden 871/871, bytecode artifacts 124/124, module cache 12/12,
Rust VM golden 774/774, and Cargo tests 162 + 3 + 2 + 2 + 7 + 13.
