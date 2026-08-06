# V5B: task-aware cooperative profile

Status: implemented on 2026-08-06 against the V5A deterministic resource and
observability contract. Cooperative hosts can opt into aggregate and per-task
execution counters without changing the existing single-task profile or CLI.

## Scope

`VM::start_cooperative_profile(quantum)` creates the existing host-controlled
FIFO session with profiling enabled before tasks are spawned.
`CooperativeRun::profile_report()` returns a snapshot containing:

- the existing `ProfileReport` as the whole-session aggregate;
- stable task-ID-ordered `TaskProfileReport` records for instruction count,
  output bytes, function calls/instructions, native calls, and source-range
  hits.

Ordinary cooperative sessions return no profile report. Profile snapshots are
non-destructive and may be inspected before execution, between dispatches, or
after terminal outcomes.

## Attribution and aggregation contract

- A bytecode instruction is charged after the normal instruction checkpoint
  succeeds and before it executes, matching the existing single-task profile.
- Function entry, native call, source-range hit, and committed output bytes are
  charged to the currently dispatched task. Synchronous native callbacks keep
  that task identity for their nested function and instruction counters.
- The aggregate instruction, output, function, native, and source-range
  counters are the deterministic sum of task execution in scheduler order.
- Tracked-heap allocation, peak-live, and estimated-byte counters remain only
  on the aggregate report. Tasks share one heap, globals, object identity, and
  retained outcomes, so per-task heap ownership would be misleading.
- A failed task retains all counters committed before its failure. Default
  fail-fast cancellation leaves an un-dispatched task at zero calls and zero
  instructions.
- Quantum expiration, queue transitions, blocking, join registration, wake,
  cancellation bookkeeping, and dispatch-boundary collection are scheduler
  operations and do not count as VM instructions.

## Compatibility boundary

`VM::profile()` and the `profile` CLI retain the existing `ProfileReport`
schema, rendering, partial-failure behavior, and output suppression. `VM::run`,
trace, debug, ordinary `start_cooperative`, and task-aware trace behavior are
unchanged. No timing metric, source syntax, artifact field, OS thread,
streaming sink, or `Send`/`Sync` promise is added; `.cdbc 0.1` remains
unchanged.

## Evidence

Focused coverage checks two-task interleaving, task-local function/output and
source-range attribution, aggregate totals, synchronous native callback
accounting, partial runtime failure, zero-count fail-fast cancellation, and the
public library export:

```sh
cargo test --manifest-path vm-rs/Cargo.toml cooperative_profile
cargo test --manifest-path vm-rs/Cargo.toml cooperative_
cargo test --manifest-path vm-rs/Cargo.toml --test library_api
python3 tests/profile_tests.py ./build/compiler_design vm-rs
git diff --check
```

## Next boundary

Task-aware debugger pauses are implemented in
[`v5b-vm-task-debugger-001.md`](v5b-vm-task-debugger-001.md). Build repeatable
multi-task workloads next; V5C concurrency expansion and V6 JIT remain gated
on their evidence and stable hot-workload characterization.
