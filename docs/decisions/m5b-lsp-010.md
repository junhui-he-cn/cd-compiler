# M5B-LSP-010: cross-module references

Status: implemented as a prototype slice.

## Decision

Extend `textDocument/references` across the opened virtual workspace. A query
first resolves a local declaration or the imported definition target already
supported by M5B-LSP-008 and M5B-LSP-009. The service then scans all collected
reference sites, matching either shared local declaration records or the same
direct-import/namespace-alias export target. With `includeDeclaration`, the
target declaration is included exactly once.

Results are converted to each source's opened URI and source-local range, then
sorted by URI and range. Snapshot-local declaration and symbol IDs remain
internal. Cross-module rename and completion are intentionally not implied by
the read-only references query.

## Migration and compatibility

The query reuses `AnalysisSnapshot`, `DeclarationIndex`,
`ReferenceSiteCollector`, the module graph, and the export-chain definition
adapter. No second reference resolver or persistent identity is introduced.
Single-document snapshots retain their previous source-local behavior and
ordering; workspace snapshots only add locations for the opened modules.

## Quantitative gate

`tests/lsp_tests.py` queries a namespace-alias member in the main document,
checks the local reference-only result, then checks the declaration-inclusive
result containing both the imported module declaration and the main-module
use. The test remains registered as the `language_server` CTest and M0A
inventory case.

## Out of scope

- cross-module rename and completion edits;
- namespace-qualified type and enum-variant navigation;
- filesystem-backed dependency discovery outside the open URI set;
- persistent semantic caching and range-based synchronization.

## Old-path deletion condition

The existing local reference path remains the shared implementation. The
workspace extension only broadens target matching and source-location mapping;
it can be removed once cross-module semantic identities are exposed directly
to every LSP query.
