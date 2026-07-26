# M3A-INTERFACE-007: dependency-ordered interface preflight

Status: implemented against reference commit `03e6083`.

## Decision

For an import-aware `Program`, the `Program::moduleGraph` is the scheduling
authority for same-process module checking. `TypeChecker` traverses each
module's import and re-export edges depth-first, checks dependencies before
their importer, and produces each dependency interface at its completed-body
boundary. `checkImport` and `checkReExport` then consume the existing
`ModuleInterface` by resolved module ID without locating or checking a
dependency `ModuleStmt` themselves.

The module body remains the current interface producer. This slice moves the
body lookup out of importer operations; it does not claim independent module
checking, serialized interface loading, or removal of dependency lowering.
The loader's cycle rejection remains authoritative, while the graph traversal
keeps an internal cycle guard for malformed snapshots.

## Compatibility and migration

Graph-aware scheduling preserves the current dependency-first load order and
all import, re-export, namespace, generic, diagnostic, IR, bytecode, `.cdbc`,
and Rust VM behavior. Direct single-file, stdin, and ordered direct multi-file
programs retain their existing paths. A focused source-metadata test reverses
the `Program::statements` vector while retaining the graph and proves that
dependency order comes from graph edges rather than AST container order.

The M0D inventory remains revision `m0d-2026-07-22-r1` with 1,745 cases.
Focused CTest, import/re-export goldens, inventory validation, and the full
parity/canonical commands form the release gate.

## Old-path deletion condition

No module body producer or lowering path is deleted here. The remaining
dependency-body producer path may be removed only after M3A/M3B defines an
independent module result and proves interface availability, visibility,
diagnostics, linked artifacts, and execution without a whole-program fallback.

## Explicitly deferred

This slice does not define interface hashes or invalidation, load serialized
interfaces, assign cross-build symbol identities, change cycle policy, or
choose between internal module objects and per-module `.cdbc` artifacts.
