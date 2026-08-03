# Compile and Runtime Benchmarks

The benchmark framework is an informational measurement tool separate from the
canonical verification matrix. It is defined by
`tests/benchmark_manifest.json` and executed by `tests/run_benchmarks.py`.

Build the two executables once, then run the default O0 measurement:

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
temporary products; load, dump, and link samples use the direct VM executable;
runtime samples repeat the last successfully prepared artifact.

| Phase | Command | Included in the sample |
| --- | --- | --- |
| compile | `compiler_design --opt-level N --emit-bytecode ...` or `--emit-module-bytecode ...` | compiler process startup, frontend/backend work, and product write |
| link | `compiler-design-vm link <module-directory> <artifact>` | module product reads, verification, deterministic expansion, and linked artifact write; module workloads only |
| load | `compiler-design-vm verify <artifact>` | VM process startup, artifact read, parse, and verification; no canonical output |
| dump | `compiler-design-vm dump <artifact>` | VM process startup, artifact read/parse/verification, and canonical formatting |
| runtime | `compiler-design-vm run <artifact>` | VM process startup, artifact load, and execution |
| IR inspection | `compiler_design --opt-level N --ir ...` | one un-timed compiler listing used to count IR instructions and virtual registers |
| bytecode inspection | `compiler_design --opt-level N --bytecode ...` | one un-timed compiler listing used to count bytecode instructions and `registerCount` |
| artifact size | final artifact from the compile/link phase | byte size of the artifact consumed by `dump` and `run` |

Inspection commands are reported separately and are not included in compile
timings. The final artifact is the directly emitted artifact for `linked`
workloads and the VM-linked artifact for `module` workloads.

## Optimization comparisons

The default command measures O0 only. Use the existing workload matrix for an
O0/O1 comparison:

```sh
python3 tests/run_benchmarks.py \
  --compare-opt-levels \
  --report build/benchmark-report-o0-o1.json
```

Alternatively, select one or more levels explicitly with repeated
`--opt-level` options, such as `--opt-level 0 --opt-level 1`. The compiler's
default O0 path is never changed by the runner.

Every selected level independently checks compile, link, load, runtime output,
runtime stderr, and exit status against the same checked-in workload contract.
When multiple levels are selected, the report also compares observed stdout,
stderr, and exit-code digests and records O0-to-O1 deltas for:

- compile, link, load, dump, and runtime median wall-clock time;
- IR and bytecode instruction counts;
- total bytecode `registerCount` across main and function bodies; and
- final artifact size in bytes.

`bytecode_metrics.register_count` is the sum of the main and function
`registerCount` values. The report also keeps main/function instruction and
register subtotals. `ir_metrics.virtual_register_count` is derived from the
highest virtual register referenced in each listed body; bytecode
`register_count` is the authoritative register-count measurement.

The report schema is version 4; the checked-in workload manifest remains
schema version 2. Timing and metric deltas are informational and do not create
a CI performance threshold. A correctness failure or an O0/O1 output/error/
exit parity failure still returns non-zero.

The `load` and `dump` samples are independent process measurements. `dump`
still performs its own parse and verification before formatting; the runner
does not subtract the two timings or claim a pure formatter CPU cost. The
separation makes parse/verify regressions visible without changing the
canonical artifact output contract.

## Workloads and reports

The checked-in manifest covers:

- startup and standalone artifact load (`artifact_load`);
- arithmetic/control flow (`arithmetic`);
- calls and closure capture (`call_closure`);
- scaled closure calls (`execution_closure`) and arithmetic dispatch
  (`execution_loop`), plus scaled array indexing (`execution_index`);
- array callback helpers and map mutation/iteration;
- native math and Unicode strings;
- the scaled public-library LFU cache workload (`library_lfu_cache`);
- the scaled public-library linked-list tail-insertion workload
  (`library_linked_list`);
- independent module emission, linking, load, dump, and execution (`module_link`);
- a deterministic runtime-error/diagnostic path (`runtime_error`).

Successful workloads use `execution: "success"` and an `expected_output` file.
Expected-failure workloads use `execution: "runtime_error"`, an
`expected_stderr` file, and an integer `expected_exit_code`; their empty stdout
is part of the contract. `artifact_mode` is `linked` for a directly emitted
program and `module` when the compiler emits independent module products that
the VM links before loading and running.

The default repetition count is three. Use `--repeat N` to override it and
`--workload NAME` (repeatable) to select a subset. The JSON report records
compile/link/load/dump/runtime samples and min/median/max seconds for each phase,
the commit and executable paths, manifest revision, selected optimization
levels, expected-output digests, observed runtime digests, host/toolchain
metadata, command templates, validation flags, timing summaries,
compiler-shape metrics, artifact sizes, and comparison deltas.

Scaled workloads deliberately run enough instructions, from hundreds of
thousands to millions, to separate VM execution changes from process startup
while staying below the default runtime resource budget. Benchmark results and
metric deltas remain informational and do not create a CI performance
threshold.

Exit code: 0 when all selected workloads and comparisons pass; non-zero when a
workload product, expected result, or requested parity check fails.
