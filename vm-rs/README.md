# Compiler Design VM

`compiler-design-vm` is the standalone Rust bytecode VM for Compiler Design `.cdbc` artifacts.

The VM-specific development plan is tracked separately in
[`docs/vm-roadmap.md`](../docs/vm-roadmap.md). The compiler and language plan
remains [`docs/roadmap.md`](../docs/roadmap.md).

## Current Commands

```sh
cargo run --manifest-path vm-rs/Cargo.toml -- --help
cargo run --manifest-path vm-rs/Cargo.toml -- dump program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run --max-steps 100000 program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- trace program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- debug program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- profile program.cdbc
```

`dump` parses and prints canonical `.cdbc` text. `run` executes the artifact
and writes program output to stdout. `trace` emits deterministic source events,
and `debug` starts the interactive breakpoint/step session.

`profile` is an opt-in machine-readable report. It prints instruction counts,
function calls/instructions, native calls, existing debug-range hits, and
output byte counts without printing the program's output. Function records are
in artifact order; native and source-range records are sorted. A runtime or
resource failure still emits the partial report before the normal diagnostic
goes to stderr. The embeddable equivalent is `vm::VM::profile()`, which
returns `ProfileRun` with both `ProfileReport` and the typed execution result.
Wall-clock timing and allocation/peak counters are intentionally deferred;
see [`docs/decisions/vm-profile-001.md`](../docs/decisions/vm-profile-001.md).

Execution and link commands use deterministic resource budgets by default. The
available overrides are `--max-steps`, `--max-call-depth`, `--max-elements`,
`--max-output-bytes`, `--max-artifact-bytes`, `--max-modules`, and
`--max-module-instructions`. A value of `0` disables one limit;
`--unlimited` disables all budgets explicitly. Embedded callers can use
`vm::CancellationToken` with `vm::RunConfig` for cooperative cancellation.
Budget failures are stable resource errors and do not emit partial `run`
stdout.

## Module Boundaries

- `bytecode`: parsed bytecode structures.
- `format`: `.cdbc` parser and serializer.
- `value`: runtime values, printing, truthiness, and equality.
- `runtime`: the `Heap` construction/identity facade over shared cells,
  environments, functions, and aggregates.
- `vm`: executor, frames, instruction dispatch, calls, and runtime errors.

Future backend tracks may add GC-aware heap ownership, task scheduling, and JIT metadata modules.
