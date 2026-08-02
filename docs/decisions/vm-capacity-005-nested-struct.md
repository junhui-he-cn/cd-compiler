# VM-5C-005: nested named-struct payload capacity boundary

Status: implemented on 2026-08-02 on top of `c06232ee`.

## Decision

Extend `tests/vm_capacity_tests.py` with an array of 1,024 `Entry` values. Each
entry owns a nested `Payload` struct containing a string and a number. The C++
compiler emits the source-backed `.cdbc` artifact, and the direct Rust VM must
report the array length and read the final nested field:

```text
1024
1023
```

The generated artifact must cross 96 KiB so this case exercises a large nested
aggregate product rather than only a small struct lookup. The exact existing
runtime-element accounting is also fixed in the test: each `Payload` costs
three units, each `Entry` costs three units, and the outer array costs 1,025
units, for 7,169 units total. Running with `--max-elements 7168` must reject
before the first `print`, while the exact `--max-elements 7169` budget must
succeed.

## Compatibility and non-goals

This slice exercises existing named-struct allocation, nested field access,
array allocation, and runtime-element accounting. It does not change
`RunConfig`, default limits, struct identity or alias semantics, cycle policy,
host RSS policy, allocator behavior, GC, or `.cdbc 0.1`.

The artifact size and elapsed/peak-RSS observations remain test evidence only;
they do not become portable host-memory thresholds.

## Evidence

The focused capacity script must pass the nested struct success and exact
budget rejection together with the existing large array/map, Unicode, output,
recursion, debug-table, and module-graph cases. Reproduce it with:

```sh
cmake -S . -B build
cmake --build build
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
ctest --test-dir build --output-on-failure -R vm_capacity
```

## Next boundary

Further aggregate scaling, allocation churn, or host-memory thresholds require
a separate workload and host/allocator policy decision. Cycle construction,
relocating handles, persistent host roots, GC, and a binary artifact remain
outside this slice.
