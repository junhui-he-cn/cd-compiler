# VM-5C-007: capacity corpus retained-byte profile observations

Status: implemented on 2026-08-02 on top of `dd966ee0`.

## Decision

Extend `tests/vm_capacity_tests.py` so the existing large-array, large-map,
nested named-struct, and aggregate-allocation-churn artifacts are each executed
twice through the opt-in Rust VM `profile` command. The test fixes these
observations for each artifact:

- the complete profile stdout is deterministic across the repeated runs;
- `tracked_heap_estimated_live_bytes` is present and positive; and
- `tracked_heap_estimated_peak_live_bytes` is present and at least the live
  estimate.

The script prints the resulting values as VM-owned representation observations
alongside its existing elapsed/peak-RSS observations.

## Compatibility and non-goals

This is a test/evidence slice over the existing VM-4B-003 profile fields. It
does not change `ProfileReport`, `.cdbc 0.1`, runtime-element charging, default
run output, failure output suppression, alias/identity semantics, or resource
diagnostics. The values are not exact allocator bytes, process RSS, a portable
host-memory limit, or a GC trigger.

The existing capacity assertions remain authoritative: successful runs and
exact budget boundaries must still pass, while budget failures must still emit
no stdout. Profile output is checked separately and never mixed with program
stdout.

## Evidence

Reproduce the focused slice with:

```sh
cmake -S . -B build
cmake --build build
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
ctest --test-dir build --output-on-failure -R vm_capacity
```

## Next boundary

The observations can guide a future workload comparison, but host RSS limits,
allocator instrumentation, cycle collection, and a relocating heap still
require a separate VM-2/VM-5 architecture decision and host consumer.
