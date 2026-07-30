# VM-5B-004: borrowed call-site locations

Status: implemented on 2026-07-30 on top of the trace-off frame-boundary
slice.

## Decision

The internal `call_function` and native callback plumbing now receives the
caller instruction's `DebugLocation` by reference. Successful calls and
callback iterations no longer clone that location. The location is cloned only
when an invalid call or runtime failure needs to own it for a diagnostic or
call-stack frame.

The borrowed location remains valid for the duration of the call because it is
owned by the immutable caller body. No artifact, `Program`, or public library
API type changes are required.

## Compatibility and non-goals

Runtime errors retain the same location and call-stack ordering, including
native callback failures. Trace, debugger, and profile behavior remain
unchanged; the optimization only changes the ownership boundary of an internal
error context. It does not alter `.cdbc 0.1`, resource limits, dispatch,
function-value layout, or native registration, and it does not introduce unsafe
code or a new artifact version.

## Evidence

The same benchmark runner and three samples were used before and after this
change. The cache and frame-boundary slices were held constant:

| Workload | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| `execution_closure` | 0.404358s | 0.393967s | -2.6% |
| `collection_helpers` | 0.001276s | 0.001404s | startup noise |
| `execution_loop` | 0.901321s | 0.860296s | no function-call attribution |

All three focused workloads passed output, stderr, and exit-status checks.
Runtime-location, callback-budget, full debugger/profile, and repository gates
also passed.
