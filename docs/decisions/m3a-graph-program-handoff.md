# M3A-GRAPH-002: graph handoff in the program snapshot

Status: implemented against reference commit `8be3e8e`.

## Decision

The import-aware graph is part of the parsed `Program` snapshot, not only a
side channel on `FrontendSession`. `Program::moduleGraph` is present exactly
when the import-aware file loader assembled module statements successfully. It
contains the same node and edge data exposed by `FrontendSession::moduleGraph()`
at that load boundary.

The graph types live in `include/ModuleGraph.hpp`, independently of the
frontend-session implementation, so later semantic and interface consumers can
accept a `Program` without depending on filesystem loading. The handoff is a
value snapshot: rebuilding or resetting a `FrontendSession` cannot mutate a
previously returned program's graph.

Stdin and ordered direct multi-file inputs that use the existing combined-entry
path leave `Program::moduleGraph` empty. They are not retroactively treated as
module graphs, and the existing source, scope, diagnostic, IR, bytecode, and VM
contracts remain unchanged.

## Migration boundary and gate

This slice changes ownership and transport only. `TypeChecker`,
`DeclarationIndex`, `IRCompiler`, bytecode emission, module-interface text, and
the Rust VM do not consume the graph yet; M3A interface/visibility slices will
migrate those consumers one at a time. The `FrontendSession` accessor remains
for loader-oriented clients and tests during the handoff.

The focused tests assert that an import-aware program carries matching module
IDs, canonical paths, and edge fields, while stdin and direct multi-file
programs carry no graph. The M0D inventory remains at revision
`m0d-2026-07-22-r1` with 1,745 cases. The frontend/session and source-metadata
CTest cases, inventory validation, and the full parity/canonical commands are
the release gate.

No old production path is deleted. The session-only graph accessor may be
retired only after all graph consumers read the `Program` snapshot and no
consumer requires filesystem-loader state. Direct-entry compatibility paths
remain deliberate and are not part of that deletion condition.

## Explicitly deferred

This slice does not materialize imported symbols from interfaces, serialize the
graph, add cache keys or per-module artifacts, change cycle diagnostics, or
alter module visibility. Those changes require separate M3A/M3B decisions and
parity gates.
