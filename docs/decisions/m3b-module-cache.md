# M3B-CACHE-001: module product cache keys and rebuild measurement

Status: implemented against the current `cdbc 0.1` module-product contract.

## Decision

The independent module-product path has a persistent, local cache manifest at
`<cache-directory>/module-cache.cdbc`. The manifest is an internal
`cdbc-cache 0.2` format and is not a VM bytecode artifact. Each record is keyed
by the graph canonical-path identity and stores:

- an exact-source `fnv1a64` digest;
- a canonical public-interface-shape digest;
- ordered import/re-export edges, requested paths, and dependency interface
  digests;
- entry status/order; and
- the module cache key, relative cached-product path, and relative `cdi 0.1`
  interface-sidecar path.

The module cache key is a length-delimited `fnv1a64` digest over the cache
schema, the `cdbc 0.1` artifact contract, canonical module identity, source and
public-interface digests, entry metadata, and ordered dependency metadata. It
does not contain snapshot-local module, source, syntax, declaration, or binding
IDs.

The cache planner applies these rules in dependency order:

- a missing record, changed source, changed module metadata, changed dependency
  edge, changed cache key, or missing product rebuilds that module;
- a source-only change with an unchanged public interface does not invalidate
  dependents;
- a public-interface change invalidates direct dependents and propagates a
  public-impact marker through all transitive dependents; and
- a cache hit copies the previously validated module product without lowering
  that module again.

The compiler validates the current entry graph and uses valid `cdi 0.1`
sidecars to preload unchanged imported interfaces before planning. A valid
sidecar skips parsing and body checking for that dependency while preserving
its source bytes and current-snapshot IDs; missing, malformed, stale, or
unpaired sidecars fall back to source parsing. This preserves diagnostics for
cache misses and keeps the product cache an artifact-level incremental slice.
The optional `--module-cache-strict` policy changes only this error handling:
it rejects an untrusted imported sidecar with an `Import` diagnostic while
entry modules continue to use source. The default remains source fallback.

`--module-rebuild-report <report.json>` records cache status, every module's
key, source/interface digests, reuse/rebuild status, reason, artifact path, and
public-impact marker. This report is the measurement boundary for the M3B
change matrix rather than an inferred aggregate hit count.

## Compatibility and migration

The default linked `--emit-bytecode` path and the independent
`--emit-module-bytecode` product keep the same cdbc 0.1 core/envelope
contract. Import-aware debug source entries may carry the additive canonical
module identity defined by M4B-DEBUG-002. Cache use is opt-in via
`--module-cache <directory>`, and a cache directory must be separate from the
module output directory. Invalid or missing manifests are treated as cold
caches, all current products are rebuilt, and a fresh manifest is written only
after successful emission.

The product cache is local and content-addressed by the stable key, but this
slice does not provide eviction, remote sharing, package resolution, or
cross-build linker symbol identities.

## Verification

- `module_cache_tests` checks deterministic keys, cache manifest and sidecar
  path round trips,
  private implementation invalidation, and direct/transitive public-interface
  invalidation.
- `tests/bytecode_module_cache_tests.py` exercises a three-module chain through
  cold build, no-change reuse, private leaf change, public leaf change, Rust
  linking, and Rust VM execution.
- The same runner compares no-cache and source-fallback output for direct,
  namespace, re-export, search-path, cold-cache, malformed-sidecar, and
  repeated-build cases.
- `frontend_session_tests` proves recursive sidecar cache hits reconstruct
  dependency graph edges and preloaded interface IDs while leaving dependency
  statement bodies empty.

Focused command:

```sh
cmake --build build --target compiler_design module_cache_tests
ctest --test-dir build --output-on-failure -R '^(frontend_session|module_cache|module_interface_artifact|bytecode_module_cache)$'
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
```

The source fallback and old whole-graph semantic path may be removed only after
independently loadable serialized interfaces preserve diagnostics and
visibility for valid, missing, malformed, stale, and changed sidecars and the
same cache change matrix passes without checking unchanged dependency bodies.
