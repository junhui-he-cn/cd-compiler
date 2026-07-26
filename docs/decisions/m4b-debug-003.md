# M4B-DEBUG-003: full source-range debug metadata

Status: implemented against the existing `cdbc 0.1` debug metadata contract.

## Decision

Keep `debug_sources` and `debug_locations` as the compatible line/column
metadata and add one optional `debug_ranges` section:

    debug_ranges:
      main 0 = s0:0:7
      function f0 2 = s0:20:25

Each entry maps an existing main/function instruction location to a source-local
half-open byte range `[start, end)`. The range uses the artifact-local `sN`
source index and is validated against the embedded source text. The compiler
derives it from the M1A1 AST `SourceRange`; it does not serialize a
snapshot-local `SourceFileId`.

The line/column section remains independently readable for old consumers.
Rust parsing and formatting retain both sections, and module linking rebases the
range source index together with the existing debug location source index while
leaving byte offsets unchanged.

## Compatibility and migration

This is an additive extension within `cdbc 0.1`. Metadata-free artifacts and
old artifacts with only `debug_sources`/`debug_locations` remain valid and
canonicalize unchanged. New compiler-emitted artifacts carry a full range for
each source-backed instruction. A range must have a matching location, the same
source index, ordered offsets, and an end offset within the source text.

Runtime diagnostics continue to use the existing line/column display. The
serialized range is now available to future debugger/runtime-event consumers
without creating a second source map.

## Quantitative gate

- C++ emitter tests cover canonical `debug_ranges` output.
- Rust tests cover round-trip parsing and out-of-bounds rejection.
- Module-product/link tests prove range sections survive product linking and
  retain one valid range per debug location.
- The existing artifact corpus remains deterministic after expected outputs are
  refreshed, and metadata-free runtime behavior remains unchanged.

## Old-path deletion condition

Keep the line/column fields and metadata-free fallback until every supported
runtime/debug consumer can use the shared range metadata or explicitly
documents why it needs only a point location. Do not remove `debug_locations`
or persist snapshot-local IDs in this slice.

## Explicitly deferred

Debugger protocol events, breakpoint tables, function symbol identities, range
compression, and a successor artifact version remain outside this slice.
