# V5B: task-aware cooperative trace

Status: implemented on 2026-08-06 against the V5A observable-ordering
contract. Cooperative hosts can opt into deterministic task-attributed trace
records without changing the existing single-task trace or CLI shape.

## Scope

`VM::start_cooperative_trace(quantum)` creates the same host-controlled FIFO
session as `start_cooperative`, with task trace collection enabled before any
task is spawned or dispatched. `TaskTraceEvent` retains the existing trace
kind, function, instruction, source location, stack, locals, and value fields
and adds the producing `TaskId`.

`CooperativeRun::trace_events()` inspects retained events and
`take_trace_events()` drains them. Draining does not reset the session event
sequence. Ordinary cooperative sessions do not collect trace events.

## Ordering and task-state contract

- Each task owns an independent trace stack and last-location stack. FIFO
  quantum interleaving cannot place another task's frame in an event.
- `Enter`, changed-source `Line`, `Output`, `Return`, `Exit`, and runtime
  `Error` observations are emitted in committed scheduler order.
- Trace and output use one session sequence. An `Output` trace event and its
  corresponding `TaskOutputEvent` share the same sequence rather than
  representing two scheduler commits.
- A normal return emits `Return` followed by `Exit`. Runtime failure emits
  `Error` and `Exit` while each active frame is still available, from the
  failing frame outward.
- Default fail-fast cancellation does not invent execution events for a task
  that is cancelled without another dispatch. Its typed task outcome remains
  the authoritative cancellation record.
- Sequence and stack state remain deterministic across repeated runs with the
  same artifact, quantum, spawn order, and host wake sequence.

## Compatibility boundary

`VM::trace()` and the `trace` CLI retain their existing `TraceEvent` schema,
sequence numbering, rendering, output result, and runtime diagnostics.
`VM::run`, `debug`, `profile`, and ordinary `start_cooperative` behavior are
unchanged. No source syntax, artifact field, OS thread, streaming sink, or
`Send`/`Sync` promise is added; `.cdbc 0.1` remains unchanged.

## Evidence

Focused coverage checks two-task interleaving, per-task stacks, line/source
locations, shared output/trace sequences, synchronous native callback frames
and output, event draining, runtime failure, and the public library export:

```sh
cargo test --manifest-path vm-rs/Cargo.toml cooperative_trace
cargo test --manifest-path vm-rs/Cargo.toml cooperative_
cargo test --manifest-path vm-rs/Cargo.toml --test library_api
git diff --check
```

## Next boundary

Task-aware profile is implemented in
[`v5b-vm-task-profile-001.md`](v5b-vm-task-profile-001.md). Next define debugger
pauses that expose the selected task and scheduler queue state while stopping
all dispatch. V5C concurrency expansion and V6 JIT remain gated on repeatable
multi-task workloads and stable hot-workload evidence.
