# VM-5B-001: trace-off instruction preamble fast path

Status: first narrow optimization implemented on 2026-07-30 after the
VM-5A-002 scaled workload baseline.

## Decision

When `run` has trace, debugger, and profile collection disabled, the VM skips
per-instruction calls that would immediately return and does not clone a
`DebugLocation` for the normal path. When any observability consumer is active,
the existing location and hook behavior remains enabled. If an instruction
fails, the error path still reconstructs the source location before attaching
the diagnostic and call stack.

The change is limited to `VM::execute_body`'s instruction preamble. Resource
checkpoints, heap observation, instruction semantics, frame construction,
native calls, and output handling remain unchanged.

## Compatibility and non-goals

The optimization does not use unsafe code, threaded dispatch, a new opcode, a
new artifact section, or a JIT. It does not alter `.cdbc 0.1`, C++ emission,
resource budgets, runtime errors, aliases, source traces, debugger pauses, or
profile counters. `trace`, `debug`, and `profile` continue to take the
observability path explicitly.

## Evidence

Compared with the 11-workload baseline on commit `3e79e3fd`, the scaled
workloads showed the useful signal while the small workloads remained startup
noise:

| Workload | Baseline runtime median | Optimized runtime median | Change |
| --- | ---: | ---: | ---: |
| `execution_loop` | 1.050626s | 0.866248s | -17.5% |
| `execution_closure` | 0.517751s | 0.452582s | -12.6% |

Both reports completed 11/11 workloads. The focused Rust VM tests, artifact
parity, debugger, profile, and full repository verification are required before
this slice is considered complete.
