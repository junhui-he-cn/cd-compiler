# VM-5B-015: profile-off hook guards

Status: implemented on 2026-07-31 on top of the shared cached frame-name
slice.

## Decision

The execution loop now calls `profile_function_entry` only when profiling is
enabled, and native dispatch calls `profile_native_call` under the same guard.
The existing hook methods and profile-enabled code remain unchanged; the
default path avoids entering a method whose first operation would return.

## Compatibility and non-goals

Profile counters, function/native ordering, trace/debug behavior, runtime
values, resource budgets, `.cdbc 0.1`, and public VM/library APIs remain
unchanged. This slice does not remove profiling, alter profile schema, or
combine profile collection with trace/debug modes.

## Evidence

The benchmark runner used base commit `8bfc5bbb` with the 009-014 changes held
constant in the working tree, manifest revision `bench-2026-07-30-r3`, three
repetitions, and checked stdout/exit contracts:

| Workload | Before median | After median | Observation |
| --- | ---: | ---: | --- |
| `collection_helpers` | 0.001185s | 0.001243s | startup noise |
| `execution_closure` | 0.254472s | 0.246518s | -3.1% |
| `native_stdlib_math` | 0.001167s | 0.001246s | startup noise |

Cargo tests and the focused benchmark workloads passed after the change.

