# Compiler Design VM

`compiler-design-vm` is the Rust VM for Compiler Design `.cdbc` bytecode
artifacts.

## Requirements

- C++17 compiler and CMake, for building the C++ compiler
- Rust stable and Cargo, for building the VM

## Build

From the repository root:

```sh
cmake -S . -B build
cmake --build build
cargo build --manifest-path vm-rs/Cargo.toml
```

Run the VM tests with:

```sh
cargo test --manifest-path vm-rs/Cargo.toml
```

## Compile and Run a Program

Compile a source file to a `.cdbc` artifact, then run it with the Rust VM:

```sh
./build/compiler_design --emit-bytecode build/program.cdbc path/to/program.cd
cargo run --manifest-path vm-rs/Cargo.toml -- run build/program.cdbc
```

Use `-I` or `--import-path` when the source imports modules outside the
source file's directory.

## Artifact versions

The C++ compiler and its default source-to-artifact path emit `cdbc 0.2`.
The Rust VM accepts valid `cdbc 0.2` artifacts and also supports explicit
`cdbc 0.3` machine artifacts through its machine-aware library formatters or
hand-built integration inputs. The 0.3 machine contract does not change the
compiler's default output; see
[`docs/decisions/cdbc-0.3-machine-abi-001.md`](../docs/decisions/cdbc-0.3-machine-abi-001.md).

## VM Commands

```sh
cargo run --manifest-path vm-rs/Cargo.toml -- verify program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- dump program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- trace program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- debug program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- profile program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- link module-directory linked.cdbc
```

- `verify` checks an artifact without executing it.
- `dump` prints canonical artifact text, including machine-aware `cdbc 0.3`
  disassembly for verified machine artifacts.
- `run` executes the program and writes its output to stdout.
- `trace` prints deterministic source events.
- `debug` starts the interactive debugger.
- `profile` prints a machine-readable execution report.
- `link` links independently emitted module artifacts.

Machine-aware debugger pauses include rendered machine registers and the active
VM frame base and size; machine integers show decimal and hexadecimal forms,
addresses show hexadecimal forms, and machine floats show decimal forms.

Show all available options with:

```sh
cargo run --manifest-path vm-rs/Cargo.toml -- --help
```

Execution limits can be set with options such as `--max-steps` and
`--max-output-bytes`; use `--unlimited` to disable all limits explicitly.
