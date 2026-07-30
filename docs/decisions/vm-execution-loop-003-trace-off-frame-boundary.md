# VM-5B-003: trace-off frame-boundary fast path

Status: implemented on 2026-07-30 on top of the lazy function-body cache
slice.

## Decision

`execute_body` now enters and leaves the trace frame only when tracing or the
debugger is enabled. Output, return, and error trace event construction also
runs only on that observability path. The default `run` path and profile-only
execution therefore avoid no-op trace calls and avoid cloning frame-entry,
frame-exit, event locations, and return-value strings.

Trace and debugger execution still use the existing frame stack and event
ordering. The guard is at the caller so the existing trace helpers remain
responsible for their enabled-state checks and data structures.

## Compatibility and non-goals

The change does not alter `.cdbc 0.1`, instruction semantics, output, runtime
errors, source locations, debugger pauses, trace event ordering, profile
counters, resource limits, or heap accounting. It does not remove any
observability path, redesign dispatch, add unsafe code, or introduce a new
artifact version. Error diagnostics continue to reconstruct their location
outside the trace-only event path.

## Evidence

The same scaled workloads were run three times before and after the change.
The cache slice was held constant; the call-heavy workload shows the intended
signal, while the arithmetic-only loop remains within ordinary benchmark
variation:

| Workload | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| `execution_closure` | 0.430038s | 0.401322s | -6.7% |
| `execution_loop` | 0.884185s | 0.892779s | +1.0% |

Both focused benchmark runs passed all correctness, stdout, stderr, and exit
status checks. Trace and profile focused tests passed before the full
repository verification gate.
