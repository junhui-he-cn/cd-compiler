# VM-5B-021: borrow the immutable entry body

Status: implemented on 2026-07-31 on top of `f6e8c9ee`.

## Decision

`VM::run_inner` now passes the verified program's immutable `main` body
directly to `execute_body`. The previous path copied the main instruction and
debug-location vectors into a temporary `FunctionBody` before the single
execution of a VM instance. The frame remains VM-local and mutable; only the
immutable body storage is borrowed from `Program`.

## Compatibility and non-goals

This changes no bytecode, register, frame, resource-budget, heap, alias,
profile, trace, debug, error, or output semantics. It does not make `Program`
mutable, cache execution state across VM instances, or alter the lazy cache for
user functions. Invalid in-memory programs continue to reach the same runtime
validation paths; normal CLI inputs remain verifier-gated.

The direct borrow is safe because `FunctionBody` is read-only during
execution. Function bodies still use the VM-local lazy cache because their
cached names and parameter vectors have a different per-call lifetime.

## Evidence

The change removes one temporary main-body instruction vector and one
temporary debug-location vector per VM execution. The existing Rust suite
passed `76 + 3 + 8` tests. A five-sample benchmark comparison checked every
output/error/exit contract:

| Workload | Baseline median | Candidate median | Observation |
| --- | ---: | ---: | --- |
| `execution_closure` | `0.250754s` | `0.257081s` | startup/run noise; no stable gain |
| `execution_loop` | `0.426355s` | `0.429915s` | startup/run noise; no stable gain |
| `collection_helpers` | `0.001126s` | `0.001203s` | startup-scale noise |
| `maps` | `0.001174s` | `0.001118s` | startup-scale noise |

The candidate benchmark also passed `artifact_load`, `native_stdlib_math`, and
`runtime_error`; the candidate run was `7/7`. The allocation reduction is the
accepted evidence for this slice, not a new timing threshold.

## Deletion condition

Keep the temporary copy only if a future mutable execution-body requirement or
an ownership audit proves that `execute_body` must own the entry vectors. A
future entry-body cache must first show repeated execution within one VM
instance or a measurable load/startup benefit; this slice does not add one.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py build/compiler_design vm-rs/target/debug/compiler-design-vm --repeat 5 --workload artifact_load --workload execution_loop --workload execution_closure --workload collection_helpers --workload maps --workload native_stdlib_math --workload runtime_error
```
