# V1B: non-moving tracing lifetime policy

Status: resolved on 2026-08-02 from the V1A baseline at `d234c665`. The
selected policy is a non-moving tracing collector. The first V1C migration
slice is implemented alongside this decision and keeps the existing runtime
value representation as its rollback boundary.

## Decision

Use a non-moving tracing collector over the current stable storage objects.
`Value` continues to carry identity-bearing `Rc<TrackedStorage<...>>` handles,
so aliases, equality, hashing, formatting, debugger values, and the `cdbc 0.1`
artifact remain unchanged. The collector does not relocate storage or expose
addresses to the artifact layer.

At a safepoint, the heap snapshots each tracked storage object and its outgoing
edges:

- environments point to binding cells;
- cells point to aggregate values or function closure environments;
- arrays, maps, and structs point to nested values;
- variants are scanned recursively for nested runtime values.

Strong references that do not originate in tracked storage are roots. This
includes VM globals and global-cell cache entries, live frame registers and
locals, native-call temporaries, returned values, and debugger-held values.
The collector marks from those roots and clears references in unmarked storage;
ordinary `Rc` destruction then releases the entire unreachable component. A
rooted cycle remains live and a cycle becomes reclaimable after its external
root is dropped.

## Runtime boundary

The first implementation provides `Heap::collect_garbage()` for embedders and
collects automatically after the top-level VM frame has ended, before `run`,
`profile`, `trace`, or `debug` returns. Collection is stop-the-world at those
boundaries, single-threaded, and conservative when a storage borrow is active.
It is not yet an incremental or concurrent collector.

Runtime errors, partial profile reports, trace events, and debugger output are
assembled before collection and retain their existing text and typed fields.
The collector only sees runtime storage; it does not mutate the bytecode
program, source tables, instruction counters, output text, or error frames.

## Rejected alternatives

| Alternative | Reason it is not selected |
| --- | --- |
| retain reference counting | Leaves supported unreachable cycles retained |
| explicit weak links | Adds language/API ownership semantics and still leaves unmarked strong cycles |
| cycle rejection | Breaks currently supported recursive values and `<cycle>` formatting |
| handle-based moving collector | Larger migration and indirection cost without a current compaction requirement |

## Compatibility and rollback

No `.cdbc` section, version, native name, public library version, or value
identity rule changes. The collector is isolated behind `Heap`; disabling the
top-level safepoint call restores the V1A retention behavior while preserving
the source and artifact corpus. Future V1C slices may replace the accounting
ledger with a dedicated mark bitset or arena, but must keep the same root,
identity, error, and debugger contracts.

## Evidence

`vm-rs/tests/cycle_lifetime.rs` covers rooted and unreachable array/map/struct,
variant-payload, callback, nested-call, closure, cancellation, and debugger
cycles, automatic reclamation, stable storage addresses, retained/dead
accounting, and repeated in-process VM profiles. `tests/vm_cycle_tests.py`
covers source emission, cycle-safe output, local and callback-cycle
reclamation, runtime errors, and profile/trace/debug determinism.

```sh
cargo test --manifest-path vm-rs/Cargo.toml --test cycle_lifetime
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/vm_cycle_tests.py ./build/compiler_design vm-rs
```

## Next boundary

The admitted V1C top-level safepoint and root-coverage slice is complete.
Collection-frequency and resource-cost measurements are still required before
adding an allocation threshold, incremental scheduling, or background work.
The storage layout and single-threaded stop-the-world boundary remain unchanged.
