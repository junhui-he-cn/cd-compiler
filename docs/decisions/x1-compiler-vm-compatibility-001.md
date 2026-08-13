# X1: Compiler/VM Compatibility Matrix

Status: resolved on 2026-08-02. This slice consolidates existing compatibility
evidence; it does not introduce a new artifact or runtime behavior.

## Decision

The current compiler/Rust VM boundary is:

| Surface | Current contract | Owner | Main evidence |
| --- | --- | --- | --- |
| linked artifact | `cdbc 0.1`; Rust validates before `dump`/`run`; `link` rejects it as a module product | shared artifact boundary | M4A matrix and artifact corpus |
| module product | `cdbc 0.1` with `artifact: module`; Rust validates and links products; standalone `run` rejects them | compiler emitter plus Rust linker | M3B/M4A and module artifact tests |
| debug metadata | optional `debug_sources`, `debug_locations`, `debug_ranges`; ranges are source-local half-open byte intervals | compiler emitter plus VM consumers | M4B, debugger, and profile tests |
| metadata-free artifacts | still valid `cdbc 0.1` inputs | Rust parser/VM | format unit and library tests |
| module cache | compiler-owned `cdbc-cache 0.2`, schema 4; source/public-interface/dependency/optimization identity, relative product paths, and product content digests | compiler only | module cache implementation and tests |
| native calls | fixed registered names; arity/callback/resource/signature metadata is VM-internal and not serialized | compiler lowering plus Rust VM | native registry and unknown-native tests |
| library/CLI | Rust library API `0.1`; CLI keeps existing dump/run/link/trace/debug/profile output and exit behavior | Rust VM | library API tests and VM README |

The complete machine-readable matrix is
[`x1-compiler-vm-compatibility-001.json`](x1-compiler-vm-compatibility-001.json).
`tests/vm_compatibility_matrix.py` checks its declarations against `VERSION`,
Cargo and library constants, artifact/cache source constants, the native
registry, the verification inventory, and every referenced evidence path.

## Compatibility rules

1. X1 changes no `cdbc` bytes, artifact version, cache schema, native name,
   CLI behavior, or VM execution semantics.
2. The compiler owns `.cdi` and module-cache invalidation/fallback. The VM
   validates and executes products and does not synthesize dependency bodies
   from compiler cache inputs.
3. Debug sections remain additive and optional. A metadata-free `cdbc 0.1`
   artifact remains a supported input.
4. A successor artifact, versioned native ABI, persistent session, GC, JIT,
   or host I/O capability needs its own decision record.

## Verification

The matrix validator and selftest are CTest cases. The behavioral contract is
covered by the existing artifact, module artifact, module cache, malformed,
Rust library, and canonical verification suites. Run the focused X1 gate:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_compatibility_matrix.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_compatibility_matrix_selftest.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/cdbc_contract_audit.py \
  ./build/compiler_design vm-rs \
  --report build/m05b-cdbc-audit-report.json
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
cargo test --manifest-path vm-rs/Cargo.toml
```

Before completing any source-backed test, rebuild the compiler. Refresh
`tests/verification_inventory.json` after adding the two CTest cases and run
the canonical repository gate from `AGENTS.md`.

## Next boundary

X1 leaves the current compatibility boundary intact. The next VM
implementation slice is `V1A`: source-backed cycle/lifetime corpus and
tracked-object/retained-byte measurement. It must stop before selecting a
cycle-collection or alternate storage strategy.
