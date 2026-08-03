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

Future backend tracks may add incremental GC scheduling, task scheduling, and
JIT metadata modules.
