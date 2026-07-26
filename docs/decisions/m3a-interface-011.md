# M3A-INTERFACE-011: opt-in strict interface-cache rejection

Status: implemented against the `M3A-INTERFACE-010` fallback matrix.

## Decision

Keep source fallback as the default policy for missing or invalid imported
interface sidecars, and add an opt-in strict policy for callers that require
cache completeness. `--module-cache-strict` requires
`--module-cache <directory>` or `--module-interface-cache <directory>`.

In strict mode, an imported sidecar is rejected with a stable `Import`
diagnostic when any of these checks fails:

- the sidecar is missing;
- the sidecar is malformed;
- its identity or canonical path does not match the requested module;
- its source hash does not match the current source bytes;
- its paired cached module product is missing; or
- a recursively loaded dependency does not provide the interface hash recorded
  by the importing sidecar.

The diagnostic has the form:

```text
Import error: module interface cache rejected for <canonical-path>: <reason>
```

The reason is one of `missing sidecar`, `malformed sidecar`, `identity/canonical
path mismatch`, `source hash mismatch`, `missing paired product`, or
`dependency interface hash mismatch`. Entry modules are always parsed from
source, even when strict mode is enabled.

## Compatibility and migration

The language, `cdi 0.1`, `cdbc 0.1`, graph ordering, and valid-sidecar public
shape are unchanged. Default cache consumers retain the M3A-INTERFACE-009
source fallback and its file-aware source diagnostics. Strict mode makes cache
trust failures explicit without changing source diagnostics, and produces no
stdout for a rejection.

This policy does not remove the default source fallback or authorize removal of
dependency-body checking. Those deletions still require the complete inventory
and the M3A/M3B interface and diagnostic gates.

## Verification

`frontend_session_tests` covers strict success plus all six rejection classes.
`tests/bytecode_module_cache_tests.py` covers CLI argument validation, strict
success, no-stdout Import diagnostics, and the same rejection matrix while
retaining the default direct/namespace/re-export/search-path fallback parity
tests.

Focused commands:

```sh
cmake --build build --target compiler_design frontend_session_tests
ctest --test-dir build --output-on-failure -R '^(frontend_session|bytecode_module_cache)$'
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
git diff --check
```

## Explicitly deferred

The policy does not add remote-cache behavior, cache repair, product-content
validation beyond the existing paired-product check, or VM-side `.cdi`
loading.
