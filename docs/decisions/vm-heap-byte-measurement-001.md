# VM-2B-002：tracked storage retained-byte measurement boundary

Status: selected on 2026-07-30 after the VM-2B workload corpus slice. This
decision measures VM-owned runtime storage without changing the `.cdbc 0.1`
contract, reference-counted aliasing, or execution results.

## Decision

`HeapStats` exposes an opt-in retained-storage estimate through
`HeapStatsSnapshot.estimated_live_bytes` and
`HeapStatsSnapshot.estimated_peak_live_bytes`. Per-kind snapshots also expose
`estimated_bytes` and `peak_estimated_bytes` for environments, cells, arrays,
maps, and structs.

The estimate includes the following VM-owned representation costs:

- the `TrackedStorage<T>` value and an `Rc` control-block allowance;
- vector capacity for array, map, struct, and inline variant payload storage;
- hash-map entry capacity for environments;
- owned field/name strings and inline string/function/variant payload storage;
- nested inline payloads without recursively following shared aggregate handles.

The ledger observes this estimate when a tracked object is allocated, when a
VM instruction completes while byte tracking is enabled, and before a stats
snapshot is returned. A mutable vector capacity change is therefore visible at
the next observation. The stats handle remains non-owning: weak ledger records
and the per-storage accounting token cannot retain a runtime object or break
the existing cycle behavior.

## Why this boundary

The VM needs a comparable pressure signal before selecting tracing collection,
relocating handles, or another backend. A scoped retained-storage estimate is
the smallest boundary that is VM-specific, reproducible for a fixed Rust
toolchain, and useful for the current workload corpus. It also exercises the
same storage objects whose live/dead and cycle behavior is already measured.

Global allocator instrumentation is deferred. A process-wide allocator counter
would include test harness, parser, CLI, and unrelated host allocations unless
it added a separate pointer ownership registry; allocator headers, bucket
control bytes, fragmentation, and RSS would still vary by host allocator. Those
numbers are useful for a later capacity study, but they are not a stable
replacement for this VM-object boundary.

## Explicit non-goals

- The estimate is not exact allocator-requested bytes, RSS, or total process
  memory. The `estimated_` prefix is part of the contract.
- `Heap` does not count ledger metadata, allocator headers, hash-map control
  bytes, or inline function/range/variant values that are not retained inside a
  tracked storage object.
- The estimate does not add garbage collection, cycle collection, relocating
  handles, persistent host roots, or a profiling file/CLI API.
- The measurement does not alter resource-budget charging; runtime element
  limits remain the deterministic safety boundary.

## Evidence

The Rust corpus covers live/dead and cyclic graphs, native temporary roots,
runtime-error and trace exits, long array churn, recursive closure pressure,
large array/map payloads, and mutable array capacity growth. The focused Rust
run after this slice is `73/73`.

```sh
cargo test --manifest-path vm-rs/Cargo.toml
```

The next backend decision must compare these tracked-object and retained-byte
measurements against the workload and benchmark requirements. It must not
infer that a tracing GC is needed from a single peak number.
