# M4B-DEBUG-001: module-product debug-source rebasing

Status: implemented against the existing `cdbc 0.1` debug metadata contract.

## Decision

Keep `debug_sources` and `debug_locations` as the current artifact metadata
representation. Treat their module-product behavior as a conformance boundary:
each module emits local source/debug tables, and the Rust linker appends those
tables in deterministic dependency expansion order while rebasing main and
function locations. A runtime failure raised in an imported function must keep
the imported source path, source line, caret location, and inner-to-outer call
stack order after linking.

No new serialized field or artifact version is needed for this proof. The
focused test uses a two-module product set, checks both source paths and both
debug sections in the linked text, runs the linked artifact twice, and compares
the complete runtime diagnostic.

## Compatibility and migration

Valid linked and module `cdbc 0.1` text remains byte-for-byte unchanged. The
test is additive and leaves metadata-free artifacts on their documented legacy
diagnostic path. Future M4B slices may add stable module identity metadata only
when a consumer needs it; this slice establishes the current path as the
baseline for that decision.

## Quantitative gate

`module_debug_metadata` is a dedicated CTest and inventory case. It must prove
that linked debug source tables contain both module paths, Rust `dump` accepts
the linked artifact through its canonical representation, and repeated `run`
invocations produce the same runtime error and frame order with no stdout.

## Old-path deletion condition

Do not remove current debug-location tables or metadata-free fallback behavior
until all compiler-emitted frames use the shared M4B metadata path and the
source-level runtime corpus has zero fallback source guesses.

## Explicitly deferred

This slice does not add module IDs to the wire format, binary framing, source
maps for a debugger protocol, breakpoint support, or a successor `cdbc` version.
