# VM-5A-001: reproducible compile/load/link/runtime benchmark boundary

Status: first workload baseline implemented on 2026-07-30 on top of the
Rust VM artifact, linker, library, debugger, profile, and diagnostic
boundaries.

## Decision

`tests/run_benchmarks.py` measures four named phases independently:

| Phase | Boundary |
| --- | --- |
| compile | C++ `compiler_design` emits either one linked `.cdbc` or a fresh module-product directory |
| link | Rust VM `link` reads the module products and writes one linked `.cdbc`; module workloads only |
| load | Rust VM `dump` reads, parses, verifies, and canonically formats the prepared artifact |
| runtime | Rust VM `run` loads and executes the prepared artifact |

The runner invokes the built VM executable directly. `cargo run` and Cargo
build time are outside every sample. Each compile/link/load sample uses fresh
temporary products; runtime samples repeat the last successfully prepared
artifact. Every phase records samples and min/median/max seconds. The report
also records the repository commit, manifest revision, source/expectation
paths and digests, host metadata, and CMake/Rust/Cargo toolchain versions.

The manifest schema is version 2. Every workload declares:

- `artifact_mode`: `linked` or `module`;
- `execution`: `success` or `runtime_error`;
- source paths;
- an expected stdout file for success, or expected stderr file and exit code
  for an expected runtime failure.

The checked-in baseline has nine workloads: arithmetic, standalone artifact
startup/load, function and closure calls, array helpers, maps, native math,
Unicode strings, independent module linking, and a deterministic out-of-range
runtime error. An expected runtime error is a passing correctness result only
when stdout is empty and stderr/exit exactly match the checked-in expectation.

## Compatibility and non-goals

This slice changes no `.cdbc` format, opcode, VM execution rule, resource
budget, diagnostic text, or language behavior. It reuses the existing `dump`
command as the load boundary; therefore the load sample includes canonical
formatting and is not an allocator/RSS measurement. Runtime timing includes
the normal `run` artifact load, while the separate load phase is an explicit
measurement point rather than a subtraction from runtime time.

Timing is informational. No threshold, baseline comparison, CI performance
gate, wall-clock field in deterministic profile reports, allocation counter,
or peak-memory claim is introduced here. The runner still returns non-zero for
incorrect products or expected results.

## Migration and deletion condition

The previous two-phase success-only runner is replaced by the schema-versioned
four-phase contract. No compatibility path is retained because benchmark
reports are measurement records, not language or artifact inputs. A future
pure parser/load command may replace `dump` only after it preserves the current
validation boundary and updates this decision plus the report schema.

## Evidence

Focused runner selftests cover manifest validation, direct executable use,
load timing, module linking, expected runtime failures, timeout handling, and
report timing summaries. The current baseline was exercised with:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks_selftest.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py --repeat 1 \
  --report build/benchmark-report-vm5a-r2.json
```

The repeat-one report completed all nine workloads successfully on the current
checkout; a normal baseline uses the manifest's default repeat count of three.
