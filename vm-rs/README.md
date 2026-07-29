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
```

`dump` parses and prints canonical `.cdbc` text. `run` executes the artifact and writes program output to stdout.

## Module Boundaries

- `bytecode`: parsed bytecode structures.
- `format`: `.cdbc` parser and serializer.
- `value`: runtime values, printing, truthiness, and equality.
- `runtime`: shared cells, environments, functions, and arrays.
- `vm`: executor, frames, instruction dispatch, calls, and runtime errors.

Future backend tracks may add GC-aware heap ownership, task scheduling, and JIT metadata modules.
