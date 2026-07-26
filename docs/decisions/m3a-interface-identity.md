# M3A-INTERFACE-001: graph-backed module-interface identity

Status: implemented against reference commit `98af1f2`.

## Decision

The in-memory `ModuleInterface` for each loaded module carries the graph-backed
source identity needed to relate public API data to the module graph:

- `moduleId` remains the snapshot-local identity shared with `ModuleStmt`;
- `sourceId` identifies the original source file in the `Program` snapshot;
- `path` remains the display path used by the existing interface text; and
- `canonicalPath` records the resolved canonical import identity.

`TypeChecker::buildModuleInterface` reads `Program::moduleGraph` by module ID
when it is available and copies the node's source and canonical identity into
the interface object. It keeps the module AST's source ID as a compatibility
fallback for manually constructed module programs. The final
`buildModuleInterfaces` pass only materializes a missing module as a defensive
compatibility path. The existing
`--module-interface` text emitter deliberately ignores these new internal
identity fields, so its output and all current CLI contracts remain unchanged.

## Migration boundary

This is an interface-object ownership slice, not separate compilation. Exported
values, generic signatures, named-struct fields/methods, enum metadata, and
current import visibility are still produced and consumed through the existing
`ModuleSymbols` and dependency-body checking path. No graph serialization,
interface file format, cache key, or Rust VM artifact field is introduced.

The focused `ir_source_location` test loads an import graph, checks the type
checker interfaces, and verifies every interface identity against the graph.
The `module_interface_emitter` test proves the stable text output remains
unchanged. The M0D inventory remains revision `m0d-2026-07-22-r1` with 1,745
cases; focused CTest, inventory validation, and the full parity/canonical
commands form the release gate.

No old production path is deleted. The AST/module-symbol identity fallback may
be removed only after every module interface is created from graph identity and
the later interface consumer slices no longer accept body-only module metadata.
The current text emitter remains authoritative until a separate serialized
interface decision is approved.

## Explicitly deferred

This slice does not make importer name/type analysis read interfaces instead of
dependency bodies, change re-export visibility, define interface versioning,
serialize canonical paths, add package resolution, or create per-module cdbc
artifacts. Those changes require later M3A/M3B and M4 decisions.
