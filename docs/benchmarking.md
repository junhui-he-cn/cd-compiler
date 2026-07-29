# Compile and Runtime Benchmarks

The benchmark framework is an informational measurement tool separate from the
canonical verification matrix. It lives in `tests/benchmark_manifest.json` and
is executed by `tests/run_benchmarks.py`.

Build the two executables once, then run the benchmark:

```sh
cmake -S . -B build
cmake --build build
cargo build --manifest-path vm-rs/Cargo.toml
python3 tests/run_benchmarks.py --report build/benchmark-report.json
```

The runner defaults to `build/compiler_design` and
`vm-rs/target/debug/compiler-design-vm`. An executable path can also be passed
as the second positional argument. The runner does not build the Rust VM and
does not use `cargo run`, so Cargo compilation is outside runtime samples.

## Measurement boundary

Each workload is measured in two independent phases:

| Phase | Command | Included in the sample |
| --- | --- | --- |
| compile | `compiler_design --emit-bytecode ...` | compiler process startup, frontend/backend work, and artifact write |
| runtime | `compiler-design-vm run <artifact>` | VM process startup and execution of the already-emitted artifact |

Compilation is repeated first, with a fresh temporary artifact for every
sample. Runtime is then repeated on the last successful artifact. A workload
fails if any compile or runtime command has a non-zero exit code, unexpected
stderr, or output that differs from its checked-in `run.out` file.

The default repetition count is three. Use `--repeat N` to override it and
`--workload NAME` (repeatable) to select a subset. The JSON report records
samples and min/median/max seconds for both phases, the commit and executable
paths, command templates, expected-output digest, host metadata, and validation
flags. Each compiler and VM subprocess has a configurable 60-second default
timeout; use `--timeout N` when a workload needs a different limit. Timing is
not compared against a baseline and does not create a CI performance gate in
this slice.
