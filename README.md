# Compiler Design

Compiler Design is an experimental programming language project with a C++17
compiler and a standalone Rust VM for `.cdbc` bytecode artifacts.

## Requirements

- C++17 compiler
- CMake
- Rust stable and Cargo
- Python 3 for the test suite

## Build

From the repository root:

```sh
cmake -S . -B build
cmake --build build
cargo build --manifest-path vm-rs/Cargo.toml
```

Run the main test suites with:

```sh
ctest --test-dir build --output-on-failure
python3 tests/run_golden_tests.py ./build/compiler_design
cargo test --manifest-path vm-rs/Cargo.toml
```

## Compile and Run Source

The compiler requires at least one source file:

```sh
./build/compiler_design examples/hello.cd
```

Useful compiler modes include:

```sh
./build/compiler_design --tokens examples/hello.cd
./build/compiler_design --ir examples/hello.cd
./build/compiler_design --ir --opt-level 1 examples/hello.cd
./build/compiler_design --bytecode examples/hello.cd
./build/compiler_design --format examples/hello.cd
./build/compiler_design --format-check examples/hello.cd
./build/compiler_design --help
```

Use `-I` or `--import-path` to add module search paths. Every source file is
an independent module: passing multiple files compiles an ordered set of entry
modules, and cross-file visibility requires `import` plus `export`.

## Compile and Run Bytecode

Emit a `.cdbc` artifact with the C++ compiler and run it with the Rust VM:

```sh
./build/compiler_design --emit-bytecode build/program.cdbc examples/hello.cd
cargo run --manifest-path vm-rs/Cargo.toml -- verify build/program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run build/program.cdbc
```

Other VM commands are:

```sh
cargo run --manifest-path vm-rs/Cargo.toml -- dump build/program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- trace build/program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- debug build/program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- profile build/program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- --help
```

Execution limits can be set with options such as `--max-steps` and
`--max-output-bytes`; use `--unlimited` to disable all limits explicitly.

## Module Artifacts

For import-aware builds, emit independent module products, link them, and run
the linked artifact:

```sh
./build/compiler_design --emit-module-bytecode build/modules main.cd
cargo run --manifest-path vm-rs/Cargo.toml -- link build/modules build/program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run build/program.cdbc
```

Pass `-I` or `--import-path` to the compiler when imported modules are in
additional directories.

## Documentation

- [Compiler developer guide (中文)](docs/compiler-developer-guide-zh.md):
  end-to-end compiler pipeline, module loading, type checking, IR, bytecode,
  diagnostics, and verification workflow for contributors.

## Tools

Start the language server as a stdio process with:

```sh
./build/compiler_design --lsp
```
