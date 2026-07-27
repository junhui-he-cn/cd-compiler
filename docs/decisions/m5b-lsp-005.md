# M5B-LSP-005: single-document rename

Status: implemented as a prototype slice.

## Decision

Expose `textDocument/rename` in the stdio LSP service for the current
single-document snapshot. The request resolves the symbol at a UTF-16
position through the shared reference query, includes the declaration range,
and returns a URI-scoped LSP `WorkspaceEdit` containing deterministic
`TextEdit[]` entries.

The requested name must be one identifier token accepted by the production
Lexer and must not be a keyword. The current document must have no diagnostics;
the source produced by applying all edits is then parsed and type-checked before
the edit is returned. Invalid names or a failed preflight return `null`.

## Migration and compatibility

Rename reuses `DeclarationIndex` reference resolution, declaration/source
ranges, and the existing full-document document state. It does not perform
textual global replacement or introduce a second name resolver. The service
still owns only one in-memory document per URI, and the client remains
responsible for applying the returned edit and sending `didChange`.

## Quantitative gate

`tests/lsp_tests.py` verifies the rename capability, a function rename with
declaration and use-site edits, and rejection of an invalid identifier. The
test remains registered as the `language_server` CTest and M0A inventory case.

## Out of scope

- cross-file or cross-module rename;
- rename conflict/code-action reporting beyond a `null` preflight result;
- prepare-rename, completion, and signature help;
- incremental range synchronization and persistent semantic caches.

## Old-path deletion condition

No editor-specific symbol resolver is introduced. Future workspace rename and
completion work must continue to consume shared semantic IDs, reference
metadata, and module interfaces before the broader M5B editor-service
boundary is complete.
