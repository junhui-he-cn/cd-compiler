# VM-5B-027: borrow the read-only `AssertArray` operand

Status: implemented on 2026-08-01 on top of commit `1fee73c7`.

## Decision

The bytecode `AssertArray` instruction now borrows its source register through
`read_register_ref`. Array and range inputs still receive the shallow owned
value required by the destination register; map inputs snapshot keys while the
map storage is borrowed and then allocate the fresh array without first
cloning the map `Value`.

## Compatibility and non-goals

Array/range iteration, insertion-ordered map-key snapshots, aliasing, runtime
element accounting, resource checkpoints, profile counters, trace/debug
locations, `.cdbc 0.1`, and CLI output remain unchanged. This is an internal
clone reduction on the map snapshot path. It does not change iterator semantics,
map storage, native callbacks, or the mutable `AssignIndex` path.

## Evidence

The existing for-in map-order regression, Rust unit/CLI/library suites, bytecode
artifact tests, Rust VM goldens, debugger/profile checks, and focused benchmark
controls pass. The `maps` workload remains output-compatible; no cross-machine
wall-clock threshold is established for this small path.

## Deletion condition

Keep the borrowed input while map iteration only snapshots keys and array/range
outputs only require a shallow destination clone. If iteration becomes
re-entrant, retains the source, or mutates the iterable during assertion,
restore an explicit ownership boundary and recheck alias/resource ordering.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_rust_vm_tests.py build/compiler_design vm-rs --goldens
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py build/compiler_design vm-rs --repeat 7 --workload maps --workload execution_loop --workload execution_index --workload execution_closure
```
