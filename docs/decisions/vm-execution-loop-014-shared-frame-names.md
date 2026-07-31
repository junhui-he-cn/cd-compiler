# VM-5B-014: shared cached frame names

Status: implemented on 2026-07-31 on top of the borrowed caller-name slice.

## Decision

Cached function bodies and execution frames now share function names through
`Rc<str>`. Creating a child frame increments the shared name handle instead of
allocating a new `String`. Trace, debugger, and runtime-error boundaries still
materialize the existing public `String` fields only when an event or stack
frame needs ownership.

The main frame retains the same visible `main` name, and cached body lookup,
function indices, and frame lifetime remain unchanged.

## Compatibility and non-goals

Trace/debug event text, debugger pauses, runtime error stack names and order,
function values, closure behavior, resource accounting, `.cdbc 0.1`, and public
VM/library APIs remain unchanged. This slice does not change public stack types,
frame registers, or the cached body eviction boundary.

## Evidence

The benchmark runner used base commit `8bfc5bbb` with the 009-013 changes held
constant in the working tree, manifest revision `bench-2026-07-30-r3`, three
repetitions, and checked stdout/exit contracts:

| Workload | Before median | After median | Observation |
| --- | ---: | ---: | --- |
| `call_closure` | 0.002010s | 0.001325s | startup noise |
| `collection_helpers` | 0.001412s | 0.001151s | startup noise |
| `execution_closure` | 0.245814s | 0.252327s | within run-to-run noise |

A seven-repetition follow-up measured `execution_closure` at `0.248311s`.
Cargo tests and the frame/debug-focused paths passed; the change is recorded as
an allocation-boundary optimization rather than a wall-clock claim.

