# V5B: scheduler control-plane foundation

Status: scheduler control-plane, explicit frame-state foundation, and the
private one-task VM adapter completed on 2026-08-03 against the V5A contract.
The public task API and multi-task host surface remain outside this slice.

## Scope

`vm-rs/src/scheduler.rs` adds a private, one-thread control plane for task
payloads plus `ResumableFrame` and `FrameStack` state. `vm-rs/src/vm.rs` adds a
private single-task adapter that executes verified bytecode through that frame
stack without changing the existing public recursive execution path.

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
runtime error payloads. The adapter now preserves output, resource checks,
native callback behavior, call-site stack locations, and dispatch-boundary
collection for its host-only path; task-aware output, trace/profile records,
debugger controls, and a public task result type remain future work.

## Compatibility boundary

The module is private to the Rust VM crate. No CLI option, `.cdbc` byte, native
name, C++ behavior, or existing single-task API changes. The current public VM
still uses its recursive function-call path. The private adapter is not wired
into `VM::run`, `trace`, `debug`, `profile`, the CLI, or the `.cdbc 0.1`
boundary. Ordinary single-task construction also avoids copying the entry
body until the adapter is explicitly requested.

## Control-plane evidence

The control-plane tests cover positive-quantum validation, FIFO tail requeue,
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

## Private adapter evidence

The adapter tests cover quantum-stable output, caller-register return
transfer, synchronous native callbacks, callback and direct-frame call-depth
budgets, instruction budgets, pre-start cancellation, nested runtime-error
stack locations, and cyclic task-root release. Each dispatch performs a heap
safepoint while the scheduler owns the suspended frame roots; terminal task
results and frames are released before the final collection.

```sh
cargo test --manifest-path vm-rs/Cargo.toml --lib cooperative_adapter
cargo test --manifest-path vm-rs/Cargo.toml
git diff --check
```

## Next slice

Add the typed multi-task host result and join/wake integration required by the
V5A contract. Keep source syntax, the CLI, `.cdbc 0.1`, and existing
single-task APIs unchanged until task-aware output, trace/profile events, and
debugger controls have an explicit compatibility contract.
