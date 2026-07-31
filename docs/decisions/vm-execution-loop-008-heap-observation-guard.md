# VM-5B-008: disabled heap-observation fast path

Status: implemented on 2026-07-31 on top of the inline common-call-arguments
slice.

## Decision

`Heap` now keeps a VM-local boolean that starts disabled. The per-instruction
`observe_estimated_bytes()` hook returns before borrowing the heap ledger until
`Heap::stats()` is requested. Calling `stats()` enables retained-byte tracking
and keeps the existing ledger observation behavior for the rest of that heap's
lifetime.

This removes the default-path `RefCell` ledger borrow used only to discover that
retained-byte measurement was disabled. It does not change allocation identity,
live/dead object counts, or the retained-byte values once measurement is
enabled.

## Compatibility and non-goals

Heap lifetime, aliasing, cycle behavior, runtime values, `.cdbc 0.1`, resource
limits, trace/debug/profile output, and public VM/library APIs remain unchanged.
The guard does not defer or sample enabled retained-byte observations, does not
replace the `Rc` storage boundary, and does not change the heap accounting
schema.

## Evidence

The benchmark runner used base commit `a5c3b86a`, manifest revision
`bench-2026-07-30-r3`, three repetitions, and checked stdout/exit contracts on
the scaled execution workloads:

| Workload | Before median | After median | Observation |
| --- | ---: | ---: | --- |
| `execution_closure` | 0.281772s | 0.286437s | within run-to-run noise |
| `execution_loop` | 0.519838s | 0.514243s | within run-to-run noise |

The result is a no-regression measurement, not a claimed wall-clock gain.
Cargo tests and the existing heap-stat tests passed, including long-array churn,
recursive closure pressure, native temporary roots, and retained-byte peak
observations. The focused benchmark workloads passed output, stderr, and exit
status checks.

