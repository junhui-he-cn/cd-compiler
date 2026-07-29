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
```

`dump` parses and prints canonical `.cdbc` text. `run` executes the artifact and writes program output to stdout.

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
