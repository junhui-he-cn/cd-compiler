# VM-5C-004: large map payload capacity boundary

Status: implemented on 2026-08-01 on top of `e390fae9`.

## Decision

Extend `tests/vm_capacity_tests.py` with a 2,048-entry map artifact. The direct
Rust VM must emit the map length and retrieve the final string key, while the
same artifact must reject a `--max-elements 1024` override before producing
stdout.

| Workload | Accepted boundary | Rejection boundary |
| --- | --- | --- |
| large map | 2,048 entries, length/key lookup succeeds | `--max-elements 1024` |

The generated artifact must cross 64 KiB so the test covers a large map
instruction/debug/source product rather than only a runtime-sized container.

## Compatibility and non-goals

This slice exercises existing map allocation, string-key lookup, output and
runtime-element accounting. It does not change `RunConfig`, map ordering or
identity semantics, artifact limits, host RSS policy, allocator behavior, GC,
or `.cdbc 0.1`.

Capacity elapsed time and peak RSS remain same-host observations only. The
1,024-element rejection is an existing runtime budget contract, not a new
portable memory limit.

## Evidence

The focused capacity script must pass the large array/map, Unicode, output,
recursion, debug-table, long-chain, diamond, and all budget rejection cases.
The full CTest, Rust VM, artifact, and canonical verification gates remain
required.

## Next boundary

Larger map payloads, map churn, or host-memory thresholds require a separate
workload and host/allocator policy decision; this test does not authorize
changing default limits.

## Reproduction

```sh
cmake -S . -B build
cmake --build build
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
ctest --test-dir build --output-on-failure -R vm_capacity
```
