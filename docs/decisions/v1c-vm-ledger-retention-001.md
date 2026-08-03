# V1C: compact dead heap-ledger entries

Status: implemented and verified on 2026-08-03. This is a narrow lifetime and
capacity follow-up to the selected top-level tracing safepoint; it does not
change collection frequency, storage identity, or the public artifact/API
boundary.

## Trigger

`HeapLedger` used one `WeakAllocation` entry per historical tracked allocation.
Dead weak entries did not retain runtime values, but they remained in the
tracing and estimated-byte scan. A long-lived embedder that explicitly reused a
`Heap` could therefore make those scans grow with historical allocation count
even after collection or ordinary reference-counted destruction.

## Change

- Keep per-kind cumulative allocation counters separately from the live weak
  allocation list, preserving `total_allocations`, `dead`, and profile counts.
- Remove dead weak entries while estimating live bytes and after explicit
  `Heap::collect_garbage()`.
- Leave live object addresses, identity values, aliasing, cycle tracing,
  resource accounting, and `cdbc 0.1` unchanged.

The compaction is bounded to the existing statistics/collection safepoints; it
does not add an allocation threshold, incremental collector, background work,
or host-memory/RSS promise.

## Evidence

Focused runtime tests assert that dead ledger entries are removed while
cumulative allocation/dead/peak counters remain unchanged. The VM regression
gate passed:

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_cycle_tests.py ./build/compiler_design vm-rs
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
PYTHONDONTWRITEBYTECODE=1 python3 tests/profile_tests.py ./build/compiler_design vm-rs
```

The same five-repeat hot-path sequence passed `4/4` before and after the
working-tree candidate. Shape, output, error, and exit-code parity remained
unchanged. The timing signal was mixed and is not claimed as a general
execution improvement. The capacity corpus observed the aggregate-allocation
churn profile phase at approximately `154.9 ms` before the change and
`21.5 ms` after it on the same host/toolchain; this is supporting evidence for
bounded historical-ledger scanning, not a portable threshold.

No new artifact bytes or versions were introduced.
