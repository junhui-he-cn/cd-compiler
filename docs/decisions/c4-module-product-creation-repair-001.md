# C4-MODULE-PRODUCT-001: strict-by-default module-product creation and repair policy

Status: implemented on 2026-08-13. Supersedes the strict/fallback default
described in `m3b-module-cache.md`; the cache key and invalidation model in
M3B-CACHE-001 is unchanged.

## Decision

Module-product builds (`--module-cache <dir>` together with
`--emit-module-bytecode <dir>`) are strict by default. Interface-only
consumers (`--module-interface-cache <dir>`) remain strict by default.
`--module-cache-fallback` is the explicit opt-out for both modes and restores
the legacy source-backed repair behavior.

The product-mode policy matrix is:

- **cold build** (no manifest file): bootstrap from source and write a fresh
  manifest; never a strict failure;
- **present-but-invalid or unreadable manifest**, or a manifest record whose
  key/artifact paths disagree with the paired sidecar: strict rejects with an
  `Import` error that names the cache directory and both repair options;
  fallback treats the cache as cold and rebuilds;
- **missing, malformed, or stale sidecars, missing manifest records, missing
  paired products, and changed source/interface/dependency/optimization
  inputs**: normal drift. The affected dependency sources are reparsed and the
  planner rebuilds, recording `cache_miss`, `source_changed`, `dependency_*`,
  `cache_artifact_missing`, or `cache_artifact_invalid` in the rebuild report.
  These are not strict failures in product mode because the pipeline can
  always regenerate correct products;
- **cached product content**: the manifest (now schema 4) stores a per-module
  `content` fnv1a64 digest of the product body. The digest is verified before
  a sidecar preload or product reuse; a mismatch rebuilds the module from
  source with reason `cache_artifact_invalid`. Semantic `.cdbc` validation
  remains the Rust linker's responsibility;
- **offline builds** stay unsupported: dependency source files must exist and
  be readable (they are opened before cache lookup), and entry modules always
  compile from source.

Interface-only consumers keep the existing trust boundary: a missing,
malformed, stale, or unpaired sidecar is rejected by default with an `Import`
diagnostic; `--module-cache-fallback` returns to the source parser. A
pre-populated cache is required.

`--module-cache-strict` remains accepted in both modes as an explicit
assertion (a no-op where strict is already the default) and stays mutually
exclusive with `--module-cache-fallback`.

## Compatibility and migration

- The manifest remains `cdbc-cache 0.2` and its internal schema moves from 3
  to 4 by adding `content = <fnv1a64>` to each module record. Schema 2 and
  schema 3 manifests are rejected as stale and treated as cold caches.
- The `--module-cache-fallback` usage error becomes
  `--module-cache-fallback requires --module-cache or --module-interface-cache`;
  the flag is no longer restricted to interface-only consumers.
- The visible behavior change: a corrupted manifest used to be silently
  discarded and rebuilt; strict-by-default now rejects it and requires an
  explicit repair (delete the cache directory or rerun with
  `--module-cache-fallback`). Normal incremental edits keep rebuilding
  transparently, and corrupted product bodies are detected and rebuilt rather
  than copied.
- No `cdbc 0.1` artifact, Rust linker, VM, or debug-metadata contract changes.

## Verification

- `tests/module_cache_tests.cpp`: schema 4 round trip, schema 2/3 cold
  migration, content-digest mismatch (`cache_artifact_invalid`), plus the
  existing key, optimization-identity, and invalidation checks.
- `tests/bytecode_module_cache_tests.py`: cold bootstrap, no-change reuse,
  private/public change matrices, invalid-manifest strict rejection and
  fallback repair, corrupted-product rebuild with correct linked execution,
  and the interface-only strict/fallback matrices.

Focused commands:

```sh
cmake --build build --target compiler_design module_cache_tests
ctest --test-dir build --output-on-failure -R '^(frontend_session|module_cache|module_interface_artifact|bytecode_module_cache)$'
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
git diff --check
```
