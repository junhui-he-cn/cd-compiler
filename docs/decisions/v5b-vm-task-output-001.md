# V5B: task-aware cooperative output

Status: implemented on 2026-08-04 against the V5A observable-ordering
contract. Cooperative hosts can attribute committed output to the task that
produced it without changing the existing single-task or CLI result shapes.

## Scope

`CooperativeRun` retains the existing dispatch-ordered string buffer and adds
typed `TaskOutputEvent` records. Each event contains a session-local monotonic
sequence, the producing `TaskId`, and the exact committed text chunk. Hosts can
inspect events with `output_events()` or drain them with
`take_output_events()` independently from `take_output()`.

This slice covers output only. Task-aware trace/profile records and debugger
pauses remain separate compatibility decisions because they carry stack,
source-location, counter, and scheduler-pause policy.

## Ordering and resource contract

- Output commits remain atomic at one bytecode `Print` instruction and use
  deterministic FIFO dispatch order.
- Event sequence numbers start at zero and remain monotonic for the session.
  Draining output bytes or event records does not reuse sequence numbers.
- `TaskOutputEvent::text` includes the exact newline appended to the ordinary
  session output, so concatenating events reconstructs the committed stream.
- The output-byte budget is cumulative across the entire session. Calling
  `take_output()` frees the host-facing buffer but does not reset resource
  charging or allow a later task to exceed `max_output_bytes`.
- A print that would exceed the shared budget commits neither output bytes nor
  an event and follows the existing deterministic fail-fast task transition.

## Compatibility boundary

`VM::run`, `trace`, `debug`, and `profile` retain their existing return types,
output text, trace shape, debugger hooks, and profile counters. No CLI option,
source syntax, artifact field, OS thread, streaming sink, or `Send`/`Sync`
promise is added. `.cdbc 0.1` remains unchanged.

## Evidence

Focused coverage checks deterministic two-task interleaving, stable task IDs
and sequences, independent byte/event draining, cumulative output limits after
a host drain, and the public library export:

```sh
cargo test --manifest-path vm-rs/Cargo.toml cooperative_
cargo test --manifest-path vm-rs/Cargo.toml --test library_api
git diff --check
```

## Next boundary

Add task identity and one scheduler event order to trace/profile observations,
then define debugger pauses that expose the selected task and scheduler queue
state while stopping all task dispatch. V5C concurrency expansion and V6 JIT
remain gated on repeatable multi-task workloads and stable hot-workload
evidence.
