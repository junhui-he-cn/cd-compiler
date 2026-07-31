# VM-5B-012: borrowed function values at call boundaries

Status: implemented on 2026-07-31 on top of the borrowed native-call name
slice.

## Decision

Internal `call_function` now receives `&FunctionValue`. Bytecode calls inspect
the function register through the borrowed register path, and native callback
helpers pass their already borrowed callback/predicate directly. The callee's
closure is cloned only when constructing the new frame; the function name is no
longer cloned for the call boundary because the cached body supplies the frame
name.

Function-value bounds and arity checks happen in the same order, and callback
argument values retain the VM-5B-007 inline `CallArguments` representation.

## Compatibility and non-goals

Closure capture and shared-cell aliasing, call depth, arity diagnostics, runtime
error stack order, callback budgets, trace/debug/profile behavior, `.cdbc 0.1`,
and public VM/library APIs remain unchanged. This slice does not alter
function-value layout, frame ownership, caller-string construction, or native
ABI.

## Evidence

The benchmark runner used base commit `8bfc5bbb` with the 009-011 changes held
constant in the working tree, manifest revision `bench-2026-07-30-r3`, three
repetitions, and checked stdout/exit contracts:

| Workload | Before median | After median | Change |
| --- | ---: | ---: | --- |
| `call_closure` | 0.001604s | 0.001509s | -5.9% |
| `collection_helpers` | 0.001470s | 0.001389s | -5.5% |
| `execution_closure` | 0.255976s | 0.249626s | -2.5% |

Cargo tests and all three focused benchmark workloads passed after the change.

