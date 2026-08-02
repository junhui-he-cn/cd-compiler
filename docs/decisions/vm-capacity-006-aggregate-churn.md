# VM-5C-006: aggregate allocation churn budget boundary

Status: implemented on 2026-08-02 on top of `68b60215`.

## Decision

Extend `tests/vm_capacity_tests.py` with a source-backed workload that replaces
three aggregate bindings 512 times:

- one one-field `Bucket` struct per iteration;
- one one-entry map per iteration; and
- one three-element array per iteration.

The initial three allocations cost 8 runtime-element units. Each iteration
costs another 8 units, so the complete workload costs `8 + 512 * 8 = 4104`
units. The direct Rust VM must produce:

```text
511
511
3
```

The default run and exact `--max-elements 4104` run must succeed. The
`--max-elements 4103` run must reject before the first `print`, proving that
runtime-element accounting remains cumulative after the previous aggregate
bindings are replaced.

## Compatibility and non-goals

This slice exercises existing array, map, and named-struct allocation in a
loop, assignment replacement, cumulative resource accounting, and failure
output suppression. It does not add garbage collection, reclaim runtime
budget units, change alias or identity semantics, add host RSS limits, or
change `RunConfig` or `.cdbc 0.1`.

Elapsed time and peak RSS remain same-host observations only. The test does not
turn them into a portable allocator or memory policy.

## Evidence

The focused capacity script must pass the churn success and exact budget
boundaries together with the existing aggregate, Unicode, output, recursion,
debug-table, and module-graph cases. Reproduce it with:

```sh
cmake -S . -B build
cmake --build build
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
ctest --test-dir build --output-on-failure -R vm_capacity
```

## Next boundary

Choosing reclamation, host-memory limits, allocator instrumentation, or a
different heap representation requires an explicit VM-2/VM-5 architecture
decision and host workload. This slice keeps cumulative runtime-element
budgeting as the deterministic safety boundary.
