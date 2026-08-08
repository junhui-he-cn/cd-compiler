# C1C-DEFAULT-001: retain O0 as the compatibility default

Status: implemented on 2026-08-03 on `feat/next-development-20260803`.

## Question

Should the compiler make O1 the default optimization level now that the
optimized debug contract and virtual-register pressure policy have been
measured?

## Decision

Keep O0 as the compatibility default. O1 remains an explicit opt-in through
`--opt-level 1` for IR, bytecode, linked artifacts, and independent module
products.

This preserves the established O0 IR/bytecode/debug output and keeps the
user-visible pipeline unchanged while O1 remains useful for controlled
measurement and experimentation. The module cache continues to distinguish
O0 and O1 through the optimization level and pipeline fingerprint; no cache
migration or default-switch invalidation is introduced.

## Evidence

The current checked-in workload matrix was measured at both levels with three
repetitions per workload at commit `e3183f2c`:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py \
  ./build/compiler_design vm-rs \
  --compare-opt-levels \
  --report build/benchmark-report-o0-o1-c1c.json
```

The result was `14 passed, 0 failed`; output, error, exit-code, and Rust VM
parity passed for `14/14` workloads.

| Evidence | Result | Interpretation |
| --- | ---: | --- |
| O0/O1 semantic and runtime parity | 14/14 | No correctness reason to change the default |
| O1 compile median direction | higher in 14/14 | The current O1 pipeline adds compile cost |
| bytecode instruction/artifact reductions | 7 lower, 7 unchanged | Shape improves for part of the matrix, not uniformly |
| runtime median direction | 7 lower, 7 higher | No stable runtime advantage on this host |
| peak live virtual-register pressure | 2 lower, 12 unchanged, 0 higher | No pressure regression, but no broad reduction |
| optimized debugger contract | passed | O1 is usable as an explicit source-backed mode |
| module-cache identity and repair | passed | O0/O1 products remain safely separated |

C1A established that O1 preserves source-backed semantic trace/debug events
while allowing ordinary line events and instruction numbers to change.
C1B established that O1 can retain virtual registers, while large products
can still grow emitted register slots. Together with the mixed runtime and
compile evidence above, these results support compatibility-first defaulting,
not an automatic O1 migration.

## Boundary and reconsideration criteria

This decision does not remove O1, change the CLI, rewrite `.cdbc 0.1`, alter
Rust VM behavior, or add a physical allocator. A future default-level review
needs new same-host evidence showing a durable compile/runtime or artifact
benefit, plus an explicit migration plan for default cache identity,
debugger expectations, and user-visible output changes.
