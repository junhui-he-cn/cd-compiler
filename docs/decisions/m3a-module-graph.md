# M3A-GRAPH-001: explicit import graph

Status: implemented against reference commit `d41760d`.

## Purpose and scope

This slice makes the import-aware `FrontendSession` loader publish an explicit
module graph beside the existing `ParsedUnit` and `ModuleStmt` representations.
It establishes the identity and dependency boundary needed by later interface
and separate-compilation work without changing name visibility, type checking,
IR lowering, bytecode emission, or runtime behavior.

## Contract

`FrontendSession::moduleGraph()` returns a snapshot-local `ModuleGraph` after a
successful import-aware `loadFiles` call. Each node records:

- the module ID already used by `ModuleStmt`, `ImportStmt`, and `ExportStmt`;
- the source-file ID used by source metadata and diagnostics;
- the display path retained for diagnostics;
- the normalized existing canonical path used for duplicate-import identity; and
- whether the module is one of the direct entry files.

The loader assigns IDs in deterministic dependency-first load order. Canonical
path de-duplication continues to produce one node for repeated spellings of the
same file. The graph keeps one edge per import declaration and one edge per
source-bearing re-export declaration. An edge records its importing module,
resolved target module, `Import` or `ReExport` kind, and the decoded path from
the source declaration.

The graph is rebuilt only after all import-aware files have loaded successfully.
Existing cycle detection still rejects a cycle before a `Program` is assembled.
For stdin and direct multi-file inputs without imports, the existing combined
entry-program path remains authoritative and the graph is empty; this slice does
not reinterpret those inputs as modules.

## Migration boundary

Graph construction runs beside the existing loader and AST assembly. Current
`TypeChecker`, `DeclarationIndex`, `IRCompiler`, bytecode compiler, module
interface emitter, diagnostics, and Rust VM paths continue to consume their
existing inputs. Visibility, exported signatures, interface serialization,
cycle policy changes, graph persistence, and dependency-body removal are later
M3A/M3B slices.

The graph is an in-memory snapshot view. Its IDs are not persistent cache keys,
artifact fields, or user-facing diagnostic coordinates. Canonical paths remain
the current import de-duplication identity, while display paths and source IDs
continue to preserve the established diagnostic/source-origin behavior.

## Gate and deletion condition

The focused `frontend_session` test checks node and edge cardinality, entry
markers, source origins, import/re-export edge kinds, endpoint references, and
field-for-field stability across equivalent search-path loads. The M0D
inventory revision remains `m0d-2026-07-22-r1` and currently contains 1,745
cases. The focused CTest, inventory validation, all legacy parity commands, and
the canonical verification report are the release gate.

No old production path is deleted in this slice. The graph becomes authoritative
for imported-module identity only after the later M3A interface/visibility
consumers read graph nodes and edges without falling back to dependency AST
bodies. Direct single-file, stdin, and ordered direct-multi-file entry-program
adapters remain deliberate compatibility paths.

## Explicitly deferred

This decision does not add module-interface serialization, package manifests,
package resolution, incremental cache keys, separate module artifacts, new
cycle diagnostics, or graph-derived type/name visibility. Those decisions must
name their own interface, artifact, and compatibility gates before replacing
the current whole-program dependency-body path.
