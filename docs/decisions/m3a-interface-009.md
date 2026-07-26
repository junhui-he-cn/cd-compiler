# M3A-INTERFACE-009: sidecar fallback and diagnostic parity

Status: implemented against the `M3A-INTERFACE-008` sidecar loader.

## Decision

Treat a serialized interface as an optimization boundary, not as an
authoritative replacement for source loading when its inputs are incomplete or
inconsistent. `FrontendSession` must fall back to the ordinary module parser
when any of these conditions holds:

- the sidecar is missing or malformed;
- the paired cached module product is missing;
- the sidecar source hash does not match the current source bytes; or
- a recursively loaded dependency sidecar does not provide the interface hash
  recorded by its importer.

The fallback is per module. A valid lower dependency may remain preloaded when
an importing sidecar is rejected, and the importer is parsed normally so its
source import/re-export edges remain authoritative. If fallback parsing finds a
syntax error, the existing file-aware diagnostic path is preserved rather than
being hidden by a stale sidecar.

## Compatibility and migration

This slice changes no language or artifact syntax. It makes the M3A-008
source-fallback contract executable and reviewable: cold caches, corrupt cache
inputs, changed source, dependency interface drift, and parser diagnostics are
all explicit cases. Valid sidecar hits continue to omit dependency statement
bodies and provide the preloaded public interface; invalid hits never produce a
partial module graph.

The fallback remains intentionally present. Removing it requires the complete
inventory to demonstrate equivalent visibility and diagnostics for valid and
invalid cache states, not merely a passing no-change cache benchmark.

## Verification

`frontend_session_tests` covers missing and malformed sidecars, missing paired
products, source-hash changes, recursive dependency interface-hash mismatch,
and source parse diagnostics. The existing module artifact/cache tests and the
full M0A verification command remain the release gate.

## Old-path deletion condition

Do not delete source fallback until the complete inventory covers these cases
for direct imports, namespace imports, re-exports, search paths, and repeated
builds, with zero unexplained visibility or diagnostic differences.

## Explicitly deferred

This slice does not add cache error reporting, remote cache policy, product
content validation in `FrontendSession`, or VM-side sidecar loading.
