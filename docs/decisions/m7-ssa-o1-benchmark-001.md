# M7-SSA-BENCH-001: measured O0/O1 benchmark comparison

Status: implemented on 2026-08-02 on `feat/ssa-o1-cfg-rewrite` after
`0810d7a0`.

## Decision

Extend the existing `tests/run_benchmarks.py` runner instead of creating a
parallel performance framework. The default remains one O0 measurement for
backward-compatible use. `--compare-opt-levels` runs the same manifest under
O0 and O1; repeated `--opt-level` options support an explicitly selected set
of levels.

Each selected level keeps the existing compile/link/load/runtime correctness
checks and additionally records:

- IR instruction counts and derived virtual-register counts from `--ir`;
- bytecode instruction counts and exact main/function `registerCount` values
  from `--bytecode`; and
- the final artifact size in bytes.

Inspection commands are not included in compile timing. O0/O1 comparison
requires every level to match the same checked-in stdout, stderr, and exit
contract, and compares observed output/error/exit digests directly. Metric
deltas are evidence only; this slice does not introduce a performance
threshold or change compiler, bytecode, cache, or VM semantics.

## Evidence

The focused runner selftest and the full existing benchmark manifest passed:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks_selftest.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py \
  build/compiler_design vm-rs \
  --compare-opt-levels \
  --report build/benchmark-report-o0-o1-r3.json
```

The selftest result was `8/8`. The comparison report used the manifest's
11 workloads with three repetitions and completed `11 passed, 0 failed` at
both O0 and O1, with parity passing for all 11 workloads. Representative
shape changes were:

| Workload | IR O0 -> O1 | bytecode delta | register delta | artifact delta |
| --- | ---: | ---: | ---: | ---: |
| `arithmetic` | 10 -> 4 | -6 | 0 | -315 B |
| `call_closure` | 43 -> 39 | -4 | 0 | -272 B |
| `execution_closure` | 41 -> 37 | -4 | 0 | -268 B |
| `module_link` | 25 -> 21 | -4 | 0 | -270 B |
| `execution_loop` | 19 -> 19 | 0 | 0 | 0 B |

The scaled loop runtime median changed by `-0.95%` in this run and the
scaled closure runtime by `+0.72%`; these are measurement observations, not
claims of a stable VM speedup. The report records all phase medians and
relative deltas for later same-host comparisons.

## Boundary

This slice does not make O1 the default, allocate physical registers, rewrite
the general CFG, compare optimized debugger traces, add a CI performance gate,
or change `cdbc 0.1`. A future default-O1 decision still requires the M7
debug-local/materialization policy and the broader semantic/source-mapping
parity corpus described in the M7 optimization decision.
