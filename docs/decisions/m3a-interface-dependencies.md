# M3A-INTERFACE-002: graph dependencies in module interfaces

Status: implemented against reference commit `154a010`.

## Decision

Each in-memory `ModuleInterface` records the graph dependency declarations of
its module in source order. A `ModuleInterfaceDependency` contains the
resolved imported module ID, the `Import` or `ReExport` edge kind, and the
decoded requested path from the source declaration.

There is one interface dependency for every import declaration and every
source-bearing re-export declaration. Canonical duplicate imports still share a
graph node but retain one dependency entry per declaration. Local export lists
without a source path do not create dependencies. The graph's existing cycle
rejection remains the loader policy and is not changed here.

## Migration boundary

`TypeChecker::buildModuleInterfaces` copies dependency edges from the
`Program::moduleGraph` snapshot after matching the interface's module ID. The
existing `ModuleInterfaceEmitter` ignores dependency metadata, so
`--module-interface` text, import visibility, diagnostics, IR, bytecode, and VM
behavior remain byte-for-byte compatible.

This is the dependency envelope needed before an interface consumer can resolve
an importer against a graph without rediscovering filesystem relationships.
Exported symbol/type materialization still uses `ModuleSymbols` and dependency
AST bodies; later M3A slices own the shadow comparison and cutover.

The focused `ir_source_location` fixture covers an entry import and a
re-export edge, checking interface dependency IDs, kinds, requested paths, and
source order. The M0D inventory remains revision `m0d-2026-07-22-r1` with
1,745 cases. Focused CTest, inventory validation, and the full parity/canonical
commands form the release gate.

No old production path is deleted. Dependency metadata may become the sole
relationship source only after interface-driven importer analysis proves
equivalence and no consumer falls back to scanning dependency statements for
edges.

## Explicitly deferred

This slice does not serialize dependencies, define interface versioning or
cache keys, materialize imported symbols from interfaces, change re-export
visibility, or enable separate compilation/per-module artifacts.
