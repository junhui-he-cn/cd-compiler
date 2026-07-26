# M3A-INTERFACE-015: default-strict interface-only cache consumers

Status: implemented after the complete imported inventory gate in
`M3A-INTERFACE-014`.

## Decision

Separate interface-only cache consumption from module-product emission at the
CLI policy boundary:

- `--module-interface-cache <directory>` is strict by default when it is used
  without module-product emission. Missing, malformed, stale, identity-mismatched,
  unpaired, or dependency-inconsistent sidecars produce the existing stable
  `Import` diagnostic.
- `--module-cache-fallback` explicitly restores source fallback for an
  interface-only consumer. It requires `--module-interface-cache`, is rejected
  for module-product emission, and is mutually exclusive with
  `--module-cache-strict`.
- `--emit-module-bytecode` with `--module-cache <directory>` retains source
  fallback by default. This is required for cold cache population and repair
  after source or cache changes. `--module-cache-strict` remains an explicit
  opt-in for callers that require complete cache coverage.
- Entry modules remain source-backed in every mode.

The sidecar format, cache manifest, public-interface hash, module graph, and
Rust VM artifact contract are unchanged.

## Compatibility and migration

Existing valid sidecar hits remain unchanged. Interface-only callers that
previously relied on implicit source fallback must pass
`--module-cache-fallback`; this keeps compatibility available without making an
invalid cache look like a trusted interface by default. Module-product cold
build and repair invocations keep their existing source-backed behavior.

The existing six rejection reasons remain stable: `missing sidecar`,
`malformed sidecar`, `identity/canonical path mismatch`, `source hash mismatch`,
`missing paired product`, and `dependency interface hash mismatch`.

## Quantitative gate

The canonical inventory records 1,768 cases, including ten module-cache cases.
The new policy case covers valid default-strict hits, default rejection,
explicit fallback parity, option-combination/scope validation, and module-
product cold/repair fallback. The complete imported inventory continues to
cover 42 successful graphs and 26 diagnostic entries.

## Old-path deletion condition

This decision does not remove dependency-body checking or module-product source
fallback. Those paths may be deleted only after a separate M3A/M3B decision
proves that all required product creation and repair inputs are available from
validated artifacts while preserving direct-input semantics and diagnostics.

## Verification

```sh
cmake --build build --target compiler_design
ctest --test-dir build --output-on-failure -R '^(frontend_session|bytecode_module_cache)$'
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
python3 tests/verification_inventory.py --write
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
git diff --check
```
