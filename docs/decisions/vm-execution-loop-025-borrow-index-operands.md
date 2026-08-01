# VM-5B-025: borrow read-only `Index` operands

Status: implemented on 2026-08-01 on top of commit `352eedfe`.

## Decision

The bytecode `Index` instruction now borrows both its collection and index
registers through `read_register_ref`. The read-only `execute_index` helper and
array-index validation accept borrowed values; array/map access still clones
only the selected result, and range access still constructs its numeric result.

`AssignIndex` remains an owned/mutable path. It continues to own the collection,
index, and assigned value through validation and mutation, while sharing the
borrowed scalar index checker without retaining a borrow across mutable storage
access.

## Compatibility and non-goals

Array, map, and range indexing; missing-key and bounds errors; aliasing;
resource checkpoints; profile counters; trace/debug locations; `.cdbc 0.1`;
and CLI output remain unchanged. This is an internal read-only `Value` clone
reduction. It does not change map key equality, range arithmetic, aggregate
storage, `AssignIndex`, or native indexing helpers.

The added `execution_index` workload performs repeated array indexing under the
default instruction budget. It is evidence for the hot path and output/error
parity, not a cross-machine wall-clock threshold.

## Evidence

The Rust unit, CLI, library, bytecode-artifact, and Rust VM golden paths pass.
The focused benchmark matrix includes `execution_index`, arithmetic loop and
closure controls, maps, and runtime error; every output, stderr, and exit
contract matches its checked-in expectation.

## Deletion condition

Keep the borrowed helper boundary while `execute_index` only reads its
operands and returns an owned result. If indexing becomes re-entrant, retains an
operand, or needs mutable collection access, restore an explicit ownership
boundary and recheck alias/lifetime behavior before extending this path.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/bytecode_artifact_tests.py build/compiler_design vm-rs
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_rust_vm_tests.py build/compiler_design vm-rs --goldens
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py build/compiler_design vm-rs --repeat 7 --workload execution_index --workload execution_loop --workload execution_closure --workload maps --workload runtime_error
```
