# VM-5B-028: borrow the primitive `AssertNumber` operand

Status: implemented on 2026-08-01 on top of commit `d3efba38`.

## Decision

The bytecode `AssertNumber` instruction now borrows its source register through
`read_register_ref`. A valid number is copied as an `f64` into a new destination
`Value`; non-number inputs still resolve the instruction's diagnostic name and
return the same error before writing the destination.

## Compatibility and non-goals

Numeric compound assignment validation, custom error text, register/name
bounds errors, resource checkpoints, profile counters, trace/debug locations,
`.cdbc 0.1`, and CLI output remain unchanged. This is an internal primitive
clone reduction. It does not change numeric representation, assignment
ownership, or any aggregate/callback path.

The existing compound-assignment artifact and golden cases provide the
regression coverage. The change is too small to establish an independent
wall-clock threshold; the focused benchmark controls remain output parity
checks.

## Evidence

Cargo unit/CLI/library tests, bytecode artifact tests, Rust VM goldens, and the
execution loop/index/closure/map benchmark controls pass. Existing
`assert_number` success and diagnostic artifacts remain unchanged.

## Deletion condition

Keep the borrowed validation while `AssertNumber` only checks a primitive and
writes a new numeric value. If it begins retaining the source or validating a
compound/reference numeric type, restore an explicit ownership boundary and
revisit the representation contract.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/bytecode_artifact_tests.py build/compiler_design vm-rs
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_rust_vm_tests.py build/compiler_design vm-rs --goldens
```
