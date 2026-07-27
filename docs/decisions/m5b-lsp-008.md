# M5B-LSP-008: virtual workspace sources and cross-module definition

Status: implemented as a prototype slice.

## Decision

When the LSP has file-backed open documents, rebuild one shared semantic
snapshot from those documents through `FrontendSession::loadVirtualFiles`.
The document that triggered the update is ordered first; imports are resolved
only against the synchronized virtual-source set, so a closed dependency is a
normal import diagnostic rather than an accidental read from disk.

`textDocument/definition` keeps the existing declaration-index path for local
symbols. If an unqualified reference is an imported binding whose interface
boundary does not carry a snapshot-local declaration ID, the adapter follows
the importing module's direct import and the target module's explicit export
or export-from chain to find the source declaration. The response serializes
the target URI and source-local range; snapshot IDs remain internal.

## Migration and compatibility

The workspace reuses the production frontend, module graph, `DeclarationIndex`,
`ReferenceSiteCollector`, and TypeChecker. Each open document receives a view
of the shared snapshot filtered by its `SourceFileId`, which prevents identical
byte offsets in different modules from being confused by document queries.
Existing local definition, references, hover, completion, document-symbol,
and workspace-symbol behavior remains source-local. Namespace aliases and
cross-module references, rename, hover, and completion are not widened by this
slice.

## Quantitative gate

`tests/lsp_tests.py` opens a main document and an imported module, publishes
clean diagnostics for both, changes the main document to use a relative direct
import, and verifies that definition returns the imported URI and declaration
range. The test remains registered as the `language_server` CTest and M0A
inventory case.

## Out of scope

- filesystem-backed dependency discovery outside the open URI set;
- namespace-alias definition and cross-module references;
- workspace-wide completion and cross-module rename;
- persistent semantic caching, project roots, and file watchers;
- incremental range-based synchronization.

## Old-path deletion condition

No editor-specific type or name resolver was introduced. The temporary
export-chain lookup is limited to adapting imported interface bindings to
source declarations until shared cross-module semantic identities can be
consumed directly by all LSP navigation queries.
