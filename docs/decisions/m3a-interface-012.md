# M3A-INTERFACE-012: canonical module-cache inventory coverage

Status: implemented against the `M3A-INTERFACE-009` fallback contract and the
`M3A-INTERFACE-011` strict-cache policy.

## Decision

Make the module-cache graph matrix first-class in the M0A inventory instead of
representing it only as one aggregate CTest result. The checked-in manifest
`tests/module_cache_cases.json` records six stable case IDs:

- incremental rebuild/reuse and public-interface invalidation;
- direct-import fallback;
- namespace-import fallback;
- re-export-chain fallback;
- search-path fallback; and
- strict rejection across the cache failure reasons.

`tests/run_verification.py` reuses the existing module-cache assertions and
records each result under its manifest name. The compiler, cache policy, and
artifact formats are unchanged.

## Migration and gate

The legacy `bytecode_module_cache` CTest remains in place. The canonical runner
adds the six named results beside it, so existing focused execution continues
to be useful while the M0A inventory can attribute a failure to the graph or
cache policy slice that failed. A clean inventory refresh is required after
adding or renaming a case.

The current proof is:

```sh
python3 tests/verification_inventory.py
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
```

The observed canonical result is 1,763 passed cases, including the six named
module-cache cases. This expands the removal-readiness evidence; it does not
yet authorize removing default source fallback or dependency-body checking.

## Old-path deletion condition

Keep source fallback until the complete imported-file, visibility, diagnostic,
and repeated-build inventory is bound to stable case IDs and has zero
unexplained parity differences. This inventory slice deletes no production
fallback path.

## Explicitly deferred

This slice does not change default cache failure behavior, add remote-cache
support, validate product contents in `FrontendSession`, or load `.cdi`
sidecars in the Rust VM.
