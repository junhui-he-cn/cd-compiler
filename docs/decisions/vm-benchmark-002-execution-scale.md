# VM-5A-002: scaled execution benchmark workloads

Status: implemented on 2026-07-30 on top of baseline commit `3e79e3fd`.

## Decision

Keep the original small correctness/startup workloads and add two checked-in
`.cdbc` artifact fixtures whose execution cost is large enough to measure the
interpreter rather than only process startup:

| Workload | Fixture | Execution shape | Profile evidence |
| --- | --- | --- | --- |
| `execution_loop` | `tests/bytecode_artifacts/benchmark_execution_loop` | 200,000-iteration arithmetic loop | 2,600,010 instructions |
| `execution_closure` | `tests/bytecode_artifacts/benchmark_execution_closure` | 50,000 calls through a captured closure | 850,024 instructions, 50,000 `add` calls |

Both fixtures are normal bytecode artifact cases with C++ emitted artifacts,
Rust canonical dumps, and checked-in runtime output. They use the default
10,000,000 instruction budget, produce only one final output line, and remain
well below the budget. The benchmark manifest revision is `bench-2026-07-30-r3`.

## Compatibility and non-goals

The fixtures do not change language, artifact, native, resource, or VM
semantics. They are not performance thresholds and do not make CI reject a
timing regression. Small workloads remain in the matrix because they cover
startup, load, module link, diagnostics, and feature-specific correctness;
scaled workloads provide a separate signal for interpreter changes.

This slice does not add a benchmark database, machine normalization, a
statistical gate, or allocation/RSS measurement. A future optimizer must report
before/after samples from these workloads and preserve their output and error
contracts.

## Evidence

The scaled workload preparation and baseline were validated with:

```sh
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs \
  --case benchmark_execution_loop --case benchmark_execution_closure
python3 tests/run_benchmarks.py --report build/benchmark-report-scale-baseline.json
```

The baseline completed 11/11 workloads. The scaled runtime medians were
1.050626 seconds for `execution_loop` and 0.517751 seconds for
`execution_closure` on the recorded host/toolchain.
