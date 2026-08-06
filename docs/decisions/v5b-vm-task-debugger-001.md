# V5B: task-aware cooperative debugger

Status: implemented on 2026-08-06 against the V5A deterministic scheduling
and observability contract. Cooperative hosts can inspect task-attributed
debugger pauses without changing the existing single-task debugger or CLI.

## Scope

`VM::start_cooperative_debug(quantum, hook)` creates the existing
host-controlled FIFO session with a `CooperativeDebugHook`. Before each
bytecode instruction, the hook receives `CooperativeDebugPause` containing:

- the selected `TaskId`, function, instruction, source location, task-local
  stack, and locals;
- `CooperativeDebugState` with the running task, the remaining FIFO ready
  queue, and every task's current state in stable task-ID order.

The hook is synchronous. No other task can dispatch while `on_instruction` or
`on_error` is running. Returning `Continue` preserves normal execution;
returning `Quit` ends the cooperative debug session and deterministically
cancels every non-terminal task. `CooperativeRun::debug_quit()` distinguishes
that host-requested termination from ordinary completion.

## Stack, callback, and error contract

- Each scheduled task retains its own debugger stack across quantum
  interleaving. A pause cannot observe another task's frames or locals.
- Synchronous native callbacks remain part of the selected task. Callback
  bytecode pauses include both the scheduled caller and callback frames.
- `on_error` runs before fail-fast cancellation and receives the failing task
  plus the scheduler state that was active for its dispatch.
- The ready queue in a pause excludes the running task and preserves the next
  dispatch order. The task-state vector marks the selected task `Running` even
  though the scheduler snapshot is presented through a synchronous hook.
- Debugger stack maintenance is private to the debug session and does not
  collect task trace events or consume task trace sequence numbers. Committed
  output retains its existing task event ordering.

## Compatibility boundary

`VM::debug()` and the `debug` CLI retain their existing `DebugPause`, hook,
quit, output, and runtime-diagnostic behavior. Ordinary cooperative, trace,
and profile sessions are unchanged. No persisted pause, asynchronous resume
handle, source syntax, artifact field, OS thread, streaming protocol, or
`Send`/`Sync` promise is added; `.cdbc 0.1` remains unchanged.

## Evidence

Focused coverage checks FIFO scheduler snapshots, task-local stacks, nested
calls, synchronous native callbacks, runtime-error attribution before
fail-fast cancellation, debug quit cancellation, trace-sequence isolation, and
the public library export:

```sh
cargo test --manifest-path vm-rs/Cargo.toml cooperative_debugger
cargo test --manifest-path vm-rs/Cargo.toml cooperative_
cargo test --manifest-path vm-rs/Cargo.toml --test library_api
python3 tests/debugger_tests.py ./build/compiler_design vm-rs
git diff --check
```

## Next boundary

V5B's deterministic host concurrency observability surface is complete. Build
repeatable multi-task workloads next, then decide whether an optional V5C
concurrency expansion has demonstrated value or whether the evidence supports
starting V6A JIT hot-workload characterization.
