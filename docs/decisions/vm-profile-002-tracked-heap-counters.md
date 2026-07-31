# VM-4B-002: deterministic tracked-heap profile counters

Status: implemented on 2026-07-31 on top of commit `8f8ad8d2`.

## Decision

The opt-in `ProfileReport` now includes two deterministic storage counters:

- `tracked_heap_allocations`: allocation records created by the existing
  VM-2B ledger;
- `tracked_heap_peak_live`: the maximum simultaneous live records in that
  ledger during the execution.

The tracked ledger covers environment, cell, array, map, and named-struct
storage. The profile reads the ledger counters directly and does not enable the
retained-byte observer or walk weak allocations at every instruction. A report
is captured for both successful and failed executions, so runtime, resource,
and cancellation failures retain the counters collected before the failure.

The CLI adds one stable line after the instruction/output line:

```text
profile heap tracked_heap_allocations=... tracked_heap_peak_live=...
```

## Compatibility and non-goals

Profiling remains opt-in. Default `run`, `trace`, and `debug` behavior,
stdout/stderr separation, instruction/resource counters, trace order, aliasing,
`.cdbc 0.1`, module products, and the library's existing typed result remain
unchanged. The counters do not include inline function/range/variant values,
runtime-element charges, estimated retained bytes, host allocator bytes, RSS,
wall-clock time, or GC activity. They are storage-ledger evidence, not a full
memory profiler.

## Evidence

Library profile tests assert deterministic success and runtime-failure counts
for function environments and parameter cells. The CLI profile matrix checks
repeatability, output separation, native/source-range records, runtime failure,
resource failure, and the new heap line:

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/profile_tests.py ./build/compiler_design vm-rs
```
