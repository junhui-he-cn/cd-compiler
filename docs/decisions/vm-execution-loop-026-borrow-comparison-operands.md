# VM-5B-026: borrow primitive comparison operands

Status: implemented on 2026-08-01 on top of commit `746d1c2b`.

## Decision

The ordered bytecode comparisons now read their operands through
`read_register_ref`. Number and string comparisons operate directly on borrowed
values, avoiding the two temporary `Value` clones previously created by the
shared comparison helper.

Named-struct ordering remains an explicit ownership boundary. Struct witnesses
are invoked through `call_function`, so the helper clones the two struct values
before the re-entrant call. Witness lookup, type checks, return validation, and
all existing error paths remain unchanged.

## Compatibility and non-goals

Numeric and Unicode string ordering, named-struct witness dispatch, aliasing,
resource checkpoints, call stacks, profile counters, trace/debug locations,
`.cdbc 0.1`, and CLI output remain unchanged. This is an internal clone
reduction for primitive operands. It does not change comparison semantics,
introduce a new ordering ABI, or borrow values across a witness call.

The existing `execution_loop` workload supplies repeated numeric comparison
coverage. Its benchmark result is no-regression evidence, not a cross-machine
wall-clock threshold.

## Evidence

The Rust comparison regression, full Rust VM unit/CLI/library suites, existing
artifact and golden coverage, and focused benchmark controls all pass. The
struct-ordering golden and module sets continue to exercise witness calls.

## Deletion condition

Keep borrowed operands for primitive comparisons while the operation remains
non-reentrant and returns a new boolean. If primitive ordering gains a callback,
retained operand, or mutable value access, restore an explicit ownership
boundary and audit call-stack/resource ordering first.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py build/compiler_design vm-rs --repeat 7 --workload execution_loop --workload execution_closure --workload execution_index --workload maps --workload runtime_error
```
