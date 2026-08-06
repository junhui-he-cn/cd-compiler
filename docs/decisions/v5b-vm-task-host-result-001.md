# V5B: typed cooperative task host result and join/wake

Status: implemented on 2026-08-04 against the V5A contract. The Rust VM now
offers an additive host-only cooperative session API while preserving the
existing single-task and `.cdbc 0.1` boundaries.

## Scope

`VM::start_cooperative(quantum)` creates a deterministic one-thread
`CooperativeRun`. A host can spawn a fresh program entry task or a verified
function entry with explicit arguments, advance FIFO quanta, inspect typed
terminal outcomes, register joins, explicitly wake blocked tasks, and cancel
tasks. No source syntax, CLI mode, artifact field, or OS-thread behavior is
introduced.

## Host API contract

- `TaskId` is monotonic within one session and is diagnostic only.
- `TaskSpec::Main` creates a fresh main frame. `TaskSpec::Function` creates a
  fresh function frame with an empty closure environment; global cells remain
  shared by the session.
- `CooperativeStep::Dispatched` reports the task and resulting scheduler
  state. `Waiting` means no task is ready while at least one task is blocked;
  `Complete` means every spawned task is terminal.
- `TaskOutcome` retains `Completed(Value)`, `Failed(RuntimeError)`, or
  `Cancelled`. Results are returned in stable task-id order by `outcomes()`.
- `join(waiter, target)` blocks a ready waiter when the target is non-terminal.
  Terminal target completion, failure, or cancellation wakes waiters in join
  registration order. `JoinPoll::Ready` returns the target's typed outcome to
  the host; source-level join continuation is intentionally deferred.
- `wake` detaches a blocked task from a pending join and requeues it. `cancel`
  immediately terminates ready or blocked tasks and requests cancellation at
  the next checkpoint for a running task.
- The first dispatched task failure triggers deterministic fail-fast
  cancellation of every other non-terminal task. A scheduler-wide
  `CancellationToken` follows the same transition.
- Output remains one session buffer in dispatch order. Terminal frame stacks
  are released before the dispatch-boundary heap collection; task values and
  errors remain available through the typed outcome layer.

## Compatibility boundary

The public `VM::run`, `trace`, `debug`, and `profile` paths remain on their
existing execution and result contracts. The session is host-only and
single-threaded; it does not promise language-level async syntax, channels,
shared-memory language semantics, `Send`/`Sync`, or a JIT execution path.

## Evidence

Focused coverage includes FIFO join waiter wake order, self/duplicate join
validation, explicit wake detachment, typed completed values, typed runtime
failures, fail-fast cancellation, and existing cooperative frame parity:

```sh
cargo test --manifest-path vm-rs/Cargo.toml cooperative_
cargo test --manifest-path vm-rs/Cargo.toml scheduler
git diff --check
```

## Next boundary

Task-aware output is implemented in
[`v5b-vm-task-output-001.md`](v5b-vm-task-output-001.md), and task-aware trace
is implemented in [`v5b-vm-task-trace-001.md`](v5b-vm-task-trace-001.md).
Add task-aware profile and debugger contracts only with separate compatibility
decisions. V5C concurrency expansion and V6 JIT remain gated on repeatable
multi-task workloads and stable hot-workload evidence.
