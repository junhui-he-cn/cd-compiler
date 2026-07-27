# M5B-LSP-003: references query

Status: implemented as a prototype slice.

## Decision

Expose `textDocument/references` in the stdio LSP service for the current
single-document snapshot. The request uses the existing zero-based UTF-16
position mapping and resolves the target through `DeclarationIndex` lexical
references plus TypeChecker binding metadata.

The response is an LSP `Location[]` in deterministic source order. The
optional `context.includeDeclaration` flag defaults to false; when true, the
target declaration range is included once alongside its resolved variable,
assignment, and compound-assignment occurrences. Invalid positions,
unknown documents, and incomplete documents return an empty array.

## Migration and compatibility

The document state and semantic query path introduced by M5B-LSP-002 are
reused. The adapter performs no independent name resolution or type
inference, and it serializes only URIs and source-local ranges. Snapshot-local
declaration, symbol, and binding IDs remain internal. The single-document
stdin boundary and stdin import rejection remain unchanged.

## Quantitative gate

`tests/lsp_tests.py` verifies the references capability, a function-call
reference with `includeDeclaration: false`, and the same query with
`includeDeclaration: true`, including deterministic ranges. The test remains
registered as the `language_server` CTest and M0A inventory case.

## Out of scope

- rename and prepare-rename;
- completion, hover/type information, and signature help;
- imported or cross-module references;
- persistent semantic caches and range-based document synchronization.

## Old-path deletion condition

No duplicate reference resolver is introduced. Future LSP navigation queries
must continue to consume shared declaration/reference metadata and stable
source ranges before the broader M5B editor-service boundary is complete.
