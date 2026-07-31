# VM-5B-013: borrowed caller names at call boundaries

Status: implemented on 2026-07-31 on top of the borrowed function-value slice.

## Decision

Internal function and native callback plumbing now passes the caller function
name as `&str`. Bytecode calls and native callback iterations no longer clone
the caller name on successful execution. Runtime error paths copy the name only
when adding an owned `StackFrame`.

The same borrowed caller is valid while the child frame executes because it is
owned by the caller frame, which is independent from the VM's mutable runtime
state. Native callback helpers pass the borrow through without changing their
callback checkpoints.

## Compatibility and non-goals

Call-stack names and ordering, runtime diagnostic text, closure behavior,
callback/resource accounting, trace/debug/profile events, `.cdbc 0.1`, and
public VM/library APIs remain unchanged. This slice does not change frame
layout, error stack ownership, caller identity, or native ABI.

## Evidence

The benchmark runner used base commit `8bfc5bbb` with the 009-012 changes held
constant in the working tree, manifest revision `bench-2026-07-30-r3`, three
repetitions, and checked stdout/exit contracts:

| Workload | Before median | After median | Observation |
| --- | ---: | ---: | --- |
| `call_closure` | 0.001509s | 0.002010s | startup noise |
| `collection_helpers` | 0.001389s | 0.001412s | startup noise |
| `execution_closure` | 0.249626s | 0.245814s | -1.5% |

Cargo tests and all three focused benchmark workloads passed after the change.

