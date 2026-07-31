# VM-5B-018: borrow main-frame cached cells

Status: implemented on 2026-07-31 on top of VM-5B-017.

## Decision

Main-frame `LoadVar` and `AssignVar` now borrow the `Cell` held by the existing
canonical global-name cache for the duration of the value read or assignment.
The cache is populated on a miss exactly as before. The compatibility helper
that returns an owned `Cell` remains available for paths that need to retain the
reference, while the hot main-frame path avoids an extra `Rc` clone per access.

Global binding replacement still refreshes the cache, and closures that already
captured the old cell still observe the old binding. Local and closure lookup
keep their existing resolution order and owned-cell behavior.

## Compatibility and non-goals

Global lookup/assignment errors, shadowing and replacement, closure aliasing,
identity, register values, resource budgets, profile counters, trace/debug
events, `.cdbc 0.1`, linked/module artifacts, and CLI behavior remain unchanged.
This slice does not cache local bindings, alter environment storage, or add
unsafe register/frame specialization.

## Evidence

The focused Rust suite passed (`74 + 3 + 8` tests), including the existing
global-cache replacement and closure-retention regression. The seven-repetition
benchmark runner checked all output/error/exit contracts. Relative to the
VM-5B-017-only run (`execution_loop` `0.403762s`, `execution_closure`
`0.246968s`), two follow-up runs after this slice measured:

| Workload | Follow-up 1 | Follow-up 2 | Observation |
| --- | ---: | ---: | --- |
| `execution_closure` | 0.255573s | 0.252530s | no stable independent gain |
| `execution_loop` | 0.422649s | 0.415965s | no stable independent gain |

The slice is retained as a behavior-preserving allocation reduction with a
no-regression result; future VM-5B work must use a more discriminating
workload or measurement before claiming further benefit.

The artifact gate passed `122/122`, module artifact validation passed, Rust VM
goldens passed `786/786`, debugger validation passed, and profile validation
passed.
