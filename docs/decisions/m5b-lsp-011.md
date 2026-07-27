# M5B-LSP-011: cross-module rename edits

Status: implemented as a prototype slice.

## Decision

Extend `textDocument/rename` over the currently opened virtual workspace for
the same value targets supported by cross-module definition and references.
The service gathers the target declaration, local references, direct-import
uses, namespace-alias members, and explicit export or export-from name tokens.
It groups edits by opened URI and returns one `WorkspaceEdit` containing all
affected documents.

Before returning edits, the adapter applies the proposed replacements to an
in-memory virtual source set and re-runs the production frontend and TypeChecker
over the whole workspace. Invalid identifiers, missing exports, duplicate
bindings, and other resulting diagnostics return `null`.

## Migration and compatibility

The implementation reuses the shared definition/reference target and
`AnalysisSnapshot`; it does not create a second rename resolver. Source-local
range validation and deterministic edit ordering remain unchanged. Snapshot
IDs are never serialized. Rename remains limited to value and field targets;
types, enum variants, and namespace-qualified type syntax are not inferred from
source text.

## Quantitative gate

`tests/lsp_tests.py` renames a namespace-alias member and verifies edits in the
main document, the target declaration, and the target module's export list.
Existing single-document rename and invalid-name assertions remain in the same
protocol test. The test remains registered as the `language_server` CTest and
M0A inventory case.

## Out of scope

- namespace-qualified type and enum-variant rename;
- filesystem-backed dependency discovery outside the open URI set;
- persistent semantic caching and range-based synchronization;
- rename edits for unopened or disk-only dependencies.

## Old-path deletion condition

The local rename path now shares the workspace target/range machinery. The
workspace edit adapter can be simplified further once cross-module semantic
identities and export-token references are first-class shared metadata.
