# VM-5B-006: main-frame global cell cache

Status: implemented on 2026-07-30 on top of the borrowed name-operand slice.

## Decision

The VM keeps a per-instance, canonical-name-slot cache for bindings resolved
from the main frame's global environment. `LoadVar` and `AssignVar` reuse the
cached `Cell` after the first lookup, while `StoreVar` refreshes the slot
whenever a global binding is created or replaced. Function frames and closure
lookup keep the existing locals -> closure -> globals search order.

The cache is internal to `VM`; it does not change the bytecode operand, the
`Program` type, or the `.cdbc 0.1` artifact. It stores the same cell already
owned by the global environment, so assignment and closure aliasing continue
to observe the same mutable binding. A replacement updates the cache without
rewriting cells retained by an existing closure.

## Compatibility and non-goals

Name-index validation, undefined-variable diagnostics, declaration replacement,
assignment semantics, closure capture, heap identity, resource budgets,
profile counters, trace/debug events, and CLI behavior remain unchanged. This
slice does not cache local or captured bindings, add register specialization,
change the environment representation, or introduce unsafe dispatch.

## Evidence

The benchmark runner used the same base commit `c8b65ecb`, manifest revision
`bench-2026-07-30-r3`, three repetitions, and checked stdout/exit contracts for
the two scaled workloads:

| Workload | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| `execution_closure` | 0.386656s | 0.299741s | -22.5% |
| `execution_loop` | 0.864550s | 0.519380s | -39.9% |

The focused Rust regression verifies missing globals, cache reuse on
assignment, cache refresh on replacement, duplicate name-index normalization,
and closure retention of the old cell. After the correctness fix, the
repository validation passed CTest `35/35`, golden `828/828`, artifact
`122/122`, module cache `11/11`, Rust VM `786/786`, canonical verification
`1897/1897`, boundary `5/5`, and malformed `108/108`; debugger, profile, LSP,
and Cargo tests also passed. `cargo fmt --check` still reports pre-existing
formatting drift outside this slice and was not used to reformat unrelated
files.
