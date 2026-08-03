# V5B: scheduler control-plane foundation

Status: scheduler control-plane and explicit frame-state foundation slices
completed on 2026-08-03 against the V5A contract. The bytecode interpreter,
public task API, and scheduler task adapter remain outside this slice.

## Scope

`vm-rs/src/scheduler.rs` adds a private, one-thread control plane for opaque
task payloads plus `ResumableFrame` and `FrameStack` state. It is intentionally
independent of the current recursive VM execution path so existing `VM::run`,
`trace`, `debug`, and `profile` behavior does not change while the frame
adapter is prepared.

## Decision

- Task IDs are monotonically assigned within one scheduler and are diagnostic
  only; they do not enter `.cdbc` or cross VM boundaries.
- Tasks have explicit `Ready`, `Running`, `Blocked`, `Completed`, `Failed`,
  and `Cancelled` states.
- A FIFO ready queue is the scheduling authority. A yielded task is appended
  at the tail, and each dispatch receives the configured positive quantum.
- A dispatch callback returns `Yield`, `Block`, `Complete`, `Fail`, or
  `Cancel`; the scheduler performs the corresponding state transition.
- `wake` is explicit and deterministic. `cancel` immediately removes ready
  tasks, marks blocked tasks terminal, and records a request for a future
  running-task checkpoint.
- A dispatch with no ready task returns `None`, allowing the future VM host to
  distinguish waiting from completion without polling or wall-clock behavior.
- A resumable frame owns instruction pointer, registers, locals, closure,
  function identity, and an optional caller return target. Returning from a
  callee validates and writes the result to the caller register; returning from
  the root yields the task result.

The control plane does not itself define output, trace/profile records, GC, or
runtime error payloads. Those remain responsibilities of the VM task adapter
and the V5A contract.

## Compatibility boundary

The module is private to the Rust VM crate. No CLI option, `.cdbc` byte, native
name, C++ behavior, or existing single-task API changes. The current VM still
uses its recursive function-call path; the next slice will adapt that path to
use the explicit frame stack as scheduler payload.

## Evidence

The focused tests cover positive-quantum validation, FIFO tail requeue,
blocked/wake transitions, ready and blocked cancellation, terminal completion,
and unknown/non-blocked task diagnostics:

```sh
rustfmt --edition 2021 --check vm-rs/src/scheduler.rs
cargo test --manifest-path vm-rs/Cargo.toml scheduler
```

The focused Rust run passed 9 scheduler/frame tests. The full VM regression
suite also passed:

```sh
cargo test --manifest-path vm-rs/Cargo.toml
git diff --check
```

## Next slice

Adapt the current recursive interpreter to a task-owned frame stack behind an
internal execution path. Preserve the current single-task API until the new
adapter has output, error, debugger/profile, resource, and GC-root parity.
