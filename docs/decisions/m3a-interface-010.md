# M3A-INTERFACE-010: complete sidecar fallback graph matrix

Status: implemented against the `M3A-INTERFACE-009` fallback contract.

## Decision

Keep invalid or missing sidecars on the ordinary source path, and bind that
behavior to a graph-shaped regression matrix. The matrix covers:

- a direct import;
- a namespace import;
- a re-export chain;
- an extensionless search-path import;
- a cold cache with no sidecars;
- a populated cache whose sidecars are malformed; and
- repeated malformed-sidecar builds.

Each case compares the no-cache compiler output with the cache-enabled source
fallback output. The malformed-sidecar cases first create paired module
products and then replace every sidecar with an invalid `cdi 9.9` header, so a
cache hit cannot be confused with a cold-cache result.

## Compatibility and migration

This slice changes no language syntax, module visibility, `cdi 0.1`, or
`cdbc 0.1` format. It extends the observable proof of the existing fallback
policy across import resolution shapes and repeated builds. Valid sidecars
continue to be covered by the module-cache reuse tests; invalid sidecars keep
the parsed source authoritative and preserve the current AST/output contract.

The source fallback remains intentionally present. This matrix is evidence for
eventual old-path review, not permission to turn cache corruption into a hard
compiler failure.

## Verification

`tests/bytecode_module_cache_tests.py` runs the matrix beside the existing
cold/reuse/invalidation/link/run checks. It compares return code, stdout, and
stderr, and repeats the malformed-sidecar invocation to check deterministic
fallback output.

Focused command:

```sh
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
```

## Old-path deletion condition

Do not remove source fallback or unchanged dependency-body checking until the
complete inventory extends this parity result to all named import/export,
diagnostic, and cache-change cases and a new decision chooses the behavior for
missing or invalid cache data.

## Explicitly deferred

This slice does not add cache diagnostics, remote cache policy, sidecar content
trust beyond the existing loader checks, or a hard-failure policy for cache
corruption.
