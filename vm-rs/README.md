# Compiler Design VM

`compiler-design-vm` is the standalone Rust bytecode VM for Compiler Design `.cdbc` artifacts.

The VM-specific development plan is tracked separately in
[`docs/vm-roadmap.md`](../docs/vm-roadmap.md). The compiler and language plan
remains [`docs/roadmap.md`](../docs/roadmap.md).

## Current Commands

```sh
cargo run --manifest-path vm-rs/Cargo.toml -- --help
cargo run --manifest-path vm-rs/Cargo.toml -- verify program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- dump program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run --max-steps 100000 program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- trace program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- debug program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- profile program.cdbc
```

`verify` reads, parses, and verifies an artifact without producing stdout;
`dump` performs the same load step and prints canonical `.cdbc` text. `run` executes the artifact
and writes program output to stdout. `trace` emits deterministic source events,
and `debug` starts the interactive breakpoint/step session.

`profile` is an opt-in machine-readable report. It prints instruction counts,
function calls/instructions, native calls, existing debug-range hits, and
output byte counts plus deterministic tracked-heap allocation/peak counters
for environment, cell, array, map, and struct storage, plus estimated live and
peak retained bytes for that tracked storage, without printing the program's
output. Function records are in artifact order; native and source-range records
are sorted. A runtime or resource failure still emits the partial report before
the normal diagnostic goes to stderr. The embeddable equivalent is
`vm::VM::profile()`, which returns `ProfileRun` with both `ProfileReport` and
the typed execution result. The estimated byte fields describe VM-owned
representation pressure, not wall-clock timing, inline values, or host
allocator/RSS measurements.

Runtime storage uses a non-moving tracing collector over stable identity-bearing
objects. `vm::runtime::Heap::collect_garbage()` is available to embedders at a
safepoint, and VM execution collects after its top-level frame ends. Rooted
cycles remain live; unreachable cycles are reclaimed without changing value
identity or the `.cdbc 0.1` boundary. Incremental and concurrent collection are
not yet promised.

Execution and link commands use deterministic resource budgets by default. The
available overrides are `--max-steps`, `--max-call-depth`, `--max-elements`,
`--max-output-bytes`, `--max-artifact-bytes`, `--max-modules`, and
`--max-module-instructions`. A value of `0` disables one limit;
`--unlimited` disables all budgets explicitly. Embedded callers can use
`vm::CancellationToken` with `vm::RunConfig` for cooperative cancellation.
Budget failures are stable resource errors and do not emit partial `run`
stdout.

The host-only `VM::start_cooperative(quantum)` API creates a deterministic
single-thread session. Hosts can spawn `TaskSpec::Main` or a verified function
entry, advance FIFO quanta with `CooperativeRun::step`, inspect typed
`TaskOutcome` values, register `join` waiters, explicitly `wake` blocked tasks,
and cancel tasks. `TaskOutputEvent` attributes exact committed output chunks to
task IDs with a stable session sequence; `output_events`/`take_output_events`
are independent from the existing dispatch-ordered string buffer, and draining
that buffer does not reset the cumulative output budget.
`VM::start_cooperative_trace(quantum)` explicitly enables `TaskTraceEvent`
collection with task-local stacks and the same session sequence as output;
`trace_events`/`take_trace_events` expose the retained records.
`VM::start_cooperative_profile(quantum)` explicitly enables non-destructive
`CooperativeProfileReport` snapshots with the existing `ProfileReport` as the
session aggregate plus task-ID-ordered `TaskProfileReport` execution counters.
Instruction, function, native, source-range, and output counts are attributed
to the current task, including synchronous native callbacks; shared-heap
metrics remain aggregate because tasks share object identity.
`VM::start_cooperative_debug(quantum, hook)` invokes synchronous
`CooperativeDebugHook` pauses with the selected task, task-local stack/locals,
FIFO ready queue, and stable task states. No other task dispatches while a hook
runs; debugger quit deterministically cancels all non-terminal tasks. A failed
task triggers the V5A fail-fast transition for the remaining non-terminal
tasks.
This surface does not add language task syntax, OS threads, `Send`/`Sync`, or
changes to `.cdbc 0.1`; the existing `VM::run`, trace, debug, and profile paths
are intact.

The library exposes separate typed diagnostic domains for artifact loading
(`ArtifactError`), module linking (`LinkError`), and execution (`RuntimeError`).
Their public `kind` fields retain structured context such as artifact line,
module/dependency identity, source location, call frames, and debug sources;
the corresponding `*Kind::as_str()` methods provide stable labels without
changing CLI display text. See
[`docs/decisions/vm-diagnostics-001.md`](../docs/decisions/vm-diagnostics-001.md).

## Module Boundaries

- `bytecode`: parsed bytecode structures.
- `format`: `.cdbc` parser and serializer.
- `value`: runtime values, printing, truthiness, and equality.
- `runtime`: the `Heap` construction/identity facade over shared cells,
  environments, functions, and aggregates.
- `vm`: executor, frames, instruction dispatch, calls, and runtime errors.

Future backend tracks may add language-level task syntax, broader concurrency
primitives, and JIT metadata modules after the deterministic host scheduler
contract has workload evidence.
