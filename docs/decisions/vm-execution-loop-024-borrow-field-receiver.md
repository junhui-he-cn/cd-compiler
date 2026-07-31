# VM-5B-024: borrow the read-only `Field` receiver

Status: implemented on 2026-07-31 on top of commit `ce06c991`.

## Decision

The bytecode `Field` instruction now borrows its receiver register through
`read_register_ref` and passes `&Value` to the read-only `execute_field` helper.
The previous path cloned the receiver before looking up a named struct field.
The helper still clones only the selected field value, which is required for
the destination register to own its result after the receiver borrow ends.

## Compatibility and non-goals

Named-struct field lookup, undefined-field and non-struct errors, aliasing,
field-value ownership, profile counters, trace/debug locations, resource
checkpoints, `.cdbc 0.1`, and CLI output remain unchanged. This is an internal
receiver clone reduction; it does not change field assignment, nested field
semantics, dynamic dispatch, or struct storage.

`AssignField` remains an owned/mutable path. Indexing and native arguments are
not included because their key/receiver lifetimes and mutation behavior need a
separate audit.

## Evidence

Rust tests passed `76 + 3 + 8`; existing C++/Rust artifact and golden coverage
continues to execute named struct field access. The change removes one
read-only receiver `Value` clone per `Field` instruction. No timing threshold is
introduced because the current benchmark manifest has no scaled field-access
workload; a future field-heavy workload must be added before claiming a
wall-clock gain.

## Deletion condition

Keep the borrowed receiver while `execute_field` only reads fields and returns
an owned field value. If field lookup becomes re-entrant or retains its receiver,
restore an explicit ownership boundary and revisit the alias contract first.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/bytecode_artifact_tests.py build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py build/compiler_design vm-rs --goldens
```
