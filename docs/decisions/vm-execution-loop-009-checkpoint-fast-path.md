# VM-5B-009: cancellation-free instruction checkpoint fast path

Status: implemented on 2026-07-31 on top of the disabled heap-observation
fast path.

## Decision

Instruction checkpoints now branch once on whether a cancellation token is
configured. The normal path checks the instruction limit and increments the
counter directly; a configured cancellation token retains the existing
cancellation-first path and checked counter increment. Unlimited execution
still uses checked arithmetic so counter overflow reports the same resource
error.

`checkpoint_native()` continues to use `checkpoint_instruction()`, so native
callback iterations consume the same instruction budget and preserve the same
error ordering.

## Compatibility and non-goals

Cancellation precedence, instruction-step counts, limit messages, native
callback accounting, trace/debug/profile behavior, runtime values, `.cdbc 0.1`,
and public VM/library APIs remain unchanged. This slice does not change the
resource budget contract, batch checkpoints, unsafe dispatch, or bytecode
validation.

## Evidence

The benchmark runner used base commit `8bfc5bbb`, manifest revision
`bench-2026-07-30-r3`, three repetitions, and checked stdout/exit contracts on
the scaled execution workloads:

| Workload | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| `execution_closure` | 0.278549s | 0.265508s | -4.7% |
| `execution_loop` | 0.503744s | 0.477791s | -5.2% |

Cargo tests, artifact/module/debugger/profile tests, and the Rust VM golden
matrix passed after the change. Existing deterministic budget, cancellation,
native callback, and resource-limit cases cover the preserved branches.

