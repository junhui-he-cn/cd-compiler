# M3B-BOUNDARY-001: module-product cache is an independent lowering boundary

Status: implemented against the existing `cdbc 0.1` module-product contract.

## Decision

An imported module body may be omitted from the current AST snapshot only when
all of the following describe the same product:

- its validated `.cdi` sidecar matches the current source and dependency
  interface hashes;
- the paired module product exists; and
- the `cdbc-cache 0.2` manifest contains the matching module record, cache key,
  sidecar path, and product path.

The empty `ModuleStmt` retained for a preloaded dependency is a graph transport
node, not a source-lowerable body. `ModuleStmt::bodySourceBacked` records this
boundary, and `IRCompiler` rejects an accidental attempt to lower such a node.
Module-product emission therefore lowers only source-backed modules that the
cache planner marks for rebuild and reuses complete products for unchanged
preloaded modules.

Missing or invalid manifests, missing records, record mismatches, missing
products, stale sidecars, and changed dependency interfaces use the existing
module-product source fallback by default. `--module-cache-strict` rejects an
untrusted module-product cache instead. Interface-only consumers retain their
separate strict-by-default or explicit-fallback policy.

The default linked `--emit-bytecode` path cannot reconstruct dependency bodies
from an interface-only cache. The CLI rejects that combination, and also
rejects module-product emission with only `--module-interface-cache`; callers
must use `--emit-module-bytecode --module-cache` and the Rust linker when they
want independent cached products.

## Migration and compatibility

The C++ compiler keeps the existing linked artifact and direct single-file or
ordered direct-multi-file behavior. Independent module emission continues to
use one product per graph node with source-ordered dependency markers. The
manifest preflight is enabled only for `--module-cache`, so lower-level
interface-only tests and source-mode consumers may still use valid sidecars
without a product-cache manifest.

No `cdbc` or `cdi` wire format changes are introduced. Snapshot-local IDs stay
out of persistent cache keys and serialized products.

## Quantitative gate

The focused matrix proves:

- complete cache hits reuse all eligible products and omit dependency body
  checking/lowering;
- private and public leaf changes preserve the declared rebuild scope;
- partial sidecar fallback preserves valid lower dependencies;
- missing and malformed manifests rebuild module products from source instead
  of lowering empty preloaded nodes;
- linked Rust artifacts execute with the same output after cold and repair
  builds; and
- interface-only cache combinations that cannot provide bytecode bodies are
  rejected before compilation.

The evidence is registered as `module_cache.product_source_fallback` and is
run by `tests/bytecode_module_cache_tests.py`.

## Verification

```sh
cmake --build build --target compiler_design frontend_session_tests
ctest --test-dir build --output-on-failure -R '^(frontend_session|module_cache|module_interface_artifact|bytecode_module_cache)$'
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
```

## Old-path deletion condition

This slice removes the unsafe path that trusted a sidecar while rebuilding
without its module-cache record. Source fallback for cold, missing, malformed,
or repaired products remains intentional until a later decision proves a
strict creation/repair policy with equivalent diagnostics and direct-input
compatibility.
