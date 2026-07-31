# VM-5B-007: inline common function-call arguments

Status: implemented on 2026-07-30 on top of the main-frame global cell cache.

## Decision

Internal function calls now represent zero, one, and two arguments as the
`CallArguments` enum variants `Empty`, `One`, and `Two`. The bytecode `Call`
path uses these variants directly for the common arities; calls with three or
more arguments retain an owned `Vec<Value>` in `Many`. Native callback helpers
use the same inline representation for their one- and two-argument callback
invocations.

`call_function` still validates arity before binding parameters and creates the
same parameter cells in the same order. The enum is VM-internal and does not
change instruction operands, `.cdbc 0.1`, runtime values, or library APIs.

## Compatibility and non-goals

Function results, argument evaluation order, arity diagnostics, callback
resource checkpoints, closure capture, aliasing, runtime errors, trace/debug
events, profile counters, and resource budgets remain unchanged. Native entry
arguments continue to use the existing `Vec<Value>` contract. This slice does
not add a dependency, redesign frames, specialize registers, or change the
native ABI.

## Evidence

The benchmark runner used base commit `4d8aa7d0`, manifest revision
`bench-2026-07-30-r3`, three repetitions, and checked stdout/exit contracts:

| Workload | Before median | After median | Observation |
| --- | ---: | ---: | --- |
| `call_closure` | 0.001418s | 0.001424s | startup noise |
| `collection_helpers` | 0.001239s | 0.001274s | startup noise |
| `execution_closure` | 0.305398s | 0.292319s | -4.3% |
| `execution_loop` | 0.537173s | 0.537550s | no-call control noise |

Cargo tests, artifact/module/debugger/profile tests, and the Rust VM golden
matrix passed after the change. The existing function, callback, reduce, and
arity cases cover the inline and `Many` argument paths.

