# Compile and Runtime Benchmarks

The benchmark framework is an informational measurement tool separate from the
canonical verification matrix. It is defined by
`tests/benchmark_manifest.json` and executed by `tests/run_benchmarks.py`.

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
does not use `cargo run`, so Cargo compilation is outside all samples.

## Measurement boundary

Each workload is measured in independent phases. Compile samples use fresh
temporary products; load and link samples use the direct VM executable; runtime
samples repeat the last successfully prepared artifact.

| Phase | Command | Included in the sample |
| --- | --- | --- |
| compile | `compiler_design --emit-bytecode ...` or `--emit-module-bytecode ...` | compiler process startup, frontend/backend work, and product write |
| link | `compiler-design-vm link <module-directory> <artifact>` | module product reads, verification, deterministic expansion, and linked artifact write; module workloads only |
| load | `compiler-design-vm dump <artifact>` | VM process startup, artifact read/parse/verification, and canonical formatting |
| runtime | `compiler-design-vm run <artifact>` | VM process startup, artifact load, and execution |

The load phase intentionally uses the existing canonical `dump` boundary. Its
stdout is validated as non-empty and is not treated as the program's output;
the runtime phase remains the execution measurement. A workload fails if a
required phase has a non-zero exit code, unexpected stderr/stdout, a missing
product, or a runtime result that differs from its checked-in expectation.

The checked-in manifest covers:

- startup and standalone artifact load (`artifact_load`);
- arithmetic/control flow (`arithmetic`);
- calls and closure capture (`call_closure`);
- array callback helpers and map mutation/iteration;
- native math and Unicode strings;
- independent module emission, linking, load, and execution (`module_link`);
- a deterministic runtime-error/diagnostic path (`runtime_error`).

Successful workloads use `execution: "success"` and an `expected_output` file.
Expected-failure workloads use `execution: "runtime_error"`, an
`expected_stderr` file, and an integer `expected_exit_code`; their empty stdout
is part of the contract. `artifact_mode` is `linked` for a directly emitted
program and `module` when the compiler emits independent module products that
the VM links before loading and running.

The default repetition count is three. Use `--repeat N` to override it and
`--workload NAME` (repeatable) to select a subset. The JSON report records
compile/link/load/runtime samples and min/median/max seconds for each phase,
the commit and executable paths, manifest revision, expected-output digests,
host/toolchain metadata, command templates, and validation flags. Timings are
not compared against a baseline and do not create a CI performance gate in
this slice; correctness failures still return non-zero.
