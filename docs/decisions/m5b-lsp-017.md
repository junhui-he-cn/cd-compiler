# M5B-LSP-017: opened-workspace completion

Status: implemented as a prototype slice.

## Decision

Extend unqualified `textDocument/completion` in the opened virtual workspace
with explicit exports from every other currently opened module. Current-module
declarations and direct-import exports remain the first candidate sources;
workspace candidates are added only when no member or qualified receiver path
selected a more specific completion mode. Candidate names already present from
the local or direct-import paths are not added again. Existing item kinds,
TypeChecker signature details, deterministic ordering, and UTF-16 prefix
replacement edits remain unchanged.

Only synchronized/opened module snapshots participate. Explicit export and
export-from chains are enumerated through the existing module graph and export
resolver; private declarations, closed imports, and disk-only modules do not
contribute candidates.

## Migration and compatibility

This is an additive extension to the existing unqualified completion fallback.
Qualified enum paths, namespace aliases, and typed struct field/method
receivers retain their receiver-specific candidate sets. The LSP continues to
use the virtual workspace snapshot and does not create a second parser,
resolver, or persistent semantic cache.

## Quantitative gate

`tests/lsp_tests.py` opens a second module with `export helper;`, adds a local
`hel` binding in the main document, and verifies that the unqualified `hel`
prefix returns the local variable and opened workspace function with exact
completion kinds, signature detail, and replacement range. The test remains
registered as the `language_server` CTest and M0A inventory case.

## Out of scope

- private declarations from other opened modules;
- closed or disk-only module completion;
- workspace candidates after member or qualified receiver prefixes;
- unknown or dynamic receiver completion;
- ranking beyond local/direct-import precedence and deterministic sorting;
- persistent semantic caching and range-based synchronization.

## Old-path deletion condition

The source candidate adapter can move into shared semantic completion metadata
once workspace export identities, visibility, and candidate precedence are
first-class records consumed by all completion modes.
