# M3A-INTERFACE-008: serialized module-interface sidecars

Status: implemented in the current working tree against the `cdbc 0.1`
module-product contract.

## Decision

Persist one complete public interface beside each independently emitted module
product as a strict `cdi 0.1` sidecar. The sidecar is keyed by the canonical
module identity and stores:

- the display/canonical identity, exact source hash, entry metadata, and
  source-ordered import/re-export dependency records;
- the canonical public shape for values, generic function signatures, named
  structs and methods, enum variants, generic constraints, receiver types, and
  resolved linkage names; and
- dependency public-interface hashes needed to validate recursive preload.

The sidecar format never serializes snapshot-local module, source, syntax,
declaration, binding, or scope IDs. The module cache manifest is
`cdbc-cache 0.2` and records the relative product path plus the relative
sidecar path.

`FrontendSession::setModuleInterfaceCacheDirectory` enables sidecar loading.
`--module-interface-cache` exposes it directly, while `--module-cache` uses the
same directory automatically during module-product emission. An imported
module is preloaded only when its sidecar identity/canonical path and source
hash match, every dependency sidecar recursively matches its recorded public
hash, and the corresponding cached product exists under the stable module
cache key. The loader retains source bytes for diagnostics, reconstructs graph
edges from sidecar dependencies, and leaves the preloaded module's token and
statement bodies empty. TypeChecker remaps sidecar interfaces to the current
snapshot IDs and skips those dependency bodies.

Missing, malformed, stale, or unpaired sidecars are not trusted: the loader
falls back to ordinary source lexing/parsing and dependency resolution. Entry
modules and direct input files retain their normal source path.

## Compatibility and migration

The existing in-memory `ModuleInterface` and linked `cdbc 0.1` artifact remain
the semantic and runtime boundaries. A valid sidecar only replaces an imported
dependency's parsed body after its public metadata and graph edge contract has
been validated. Public values and methods retain the resolved names required by
the current same-process linker/lowering path. Rust VM products do not parse or
consume `.cdi` files; the sidecar is a compiler/frontend cache input.

This slice makes independent interface loading available without deleting the
source fallback. It therefore preserves diagnostics and visibility when a
cache is cold or invalid while proving the cache-hit graph/ID/public-shape
handoff separately.

## Verification

- `frontend_session_tests` creates a three-module cache hit, proves both
  dependency bodies are empty, and checks reconstructed graph edges, current
  module/source IDs, dependency metadata, public values, and linkage names.
- `module_interface_artifact_tests` proves recursive type fidelity, canonical
  public hashing, identity validation, and malformed sidecar rejection.
- `tests/bytecode_module_cache_tests.py` covers cold reuse, no-change reuse,
  private implementation invalidation, public-interface propagation, linking,
  and VM execution.

The M0D inventory remains revision `m0d-2026-07-22-r1`; the release gate also
runs CTest, golden/artifact/Rust VM tests, canonical verification, boundary and
malformed-input checks, Cargo tests, and `git diff --check`.

## Old-path deletion condition

Do not remove source fallback or the current diagnostic/visibility comparison
until the complete inventory proves equivalent results for valid sidecars,
missing sidecars, malformed sidecars, changed source, changed dependency
interfaces, and changed graph edges across repeated builds.

## Explicitly deferred

This slice does not define package registries, remote caches, cache eviction,
cross-build linker symbol allocation, VM-side `.cdi` parsing, or a new `cdbc`
artifact version.
