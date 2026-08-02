# VM-4B-003: deterministic estimated retained-byte profile fields

Status: implemented on 2026-08-02 on top of `50d2521c`.

## Decision

Extend the opt-in `ProfileReport` and `compiler-design-vm profile` output with:

- `tracked_heap_estimated_live_bytes`; and
- `tracked_heap_estimated_peak_live_bytes`.

`VM::profile()` enables the existing VM-owned `HeapStats` retained-storage
observer before execution and snapshots it after both successful and failed
runs. The fields therefore report the existing `estimated_*` representation
model across environment, cell, array, map, and struct storage. Profile output
remains deterministic for a fixed Rust build and the existing report ordering is
unchanged.

## Compatibility and non-goals

This is an opt-in profile/API observation. Default `run`, `trace`, and `debug`
do not enable retained-byte observation. The slice does not change `.cdbc 0.1`,
runtime-element charging, alias or identity semantics, stdout/stderr behavior,
or runtime/resource diagnostics.

The estimates are not exact allocator bytes, process RSS, wall-clock time,
inline function/range/variant payload accounting, a portable host-memory limit,
or a GC trigger. They measure the already documented VM-owned tracked-storage
representation and remain separate from host allocator policy.

## Evidence

The library profile tests assert non-zero live estimates and peak/live ordering
for successful and failed executions. The CLI profile test checks the new fields
and repeatability, while the existing profile, debugger, artifact, C++/Rust
golden, and resource gates preserve the unchanged execution contract.

```sh
cmake -S . -B build
cmake --build build
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/profile_tests.py ./build/compiler_design vm-rs
ctest --test-dir build --output-on-failure -R 'profile|rust_vm|debugger'
```

## Next boundary

The next heap decision must compare these VM-owned retained-byte observations
with the capacity/churn workloads before considering host RSS limits, allocator
instrumentation, cycle collection, or a relocating handle representation.
