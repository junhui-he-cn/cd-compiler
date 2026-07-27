# M5B-LSP-007: open-document workspace symbols

Status: implemented as a prototype slice.

## Decision

Expose `workspace/symbol` in the stdio LSP service for the current set of
opened document URIs. The optional query string filters declaration names by
substring. Results are LSP `SymbolInformation[]` entries with name, symbol
kind, and URI/range location, sorted deterministically by URI and source
range.

Only documents with a parseable program and a source-local declaration range
contribute results. Closing a document removes its symbols immediately. The
query does not load files, materialize imports, or claim workspace/module
semantic resolution.

## Migration and compatibility

The service reuses each document's `DeclarationIndex`, TypeChecker-backed
analysis snapshot, and source ranges. It does not serialize snapshot-local
IDs or create a second declaration collector. The existing single-document
stdin import boundary remains unchanged; open-document aggregation is a
protocol query over already synchronized state.

## Quantitative gate

`tests/lsp_tests.py` verifies the workspace-symbol capability, opens a second
document, queries its function by substring, checks the URI and range in the
returned `SymbolInformation`, and verifies close cleanup. The test remains
registered as the `language_server` CTest and M0A inventory case.

## Out of scope

- imported module loading and cross-module definition/references;
- scope-aware ranking and workspace-wide completion;
- filesystem watchers, workspace folders, and project roots;
- persistent semantic caches and range-based synchronization.

## Old-path deletion condition

No workspace-specific declaration collector is introduced. Future module
navigation must consume the existing module graph/interfaces and synchronized
source identities before replacing this open-document boundary.
