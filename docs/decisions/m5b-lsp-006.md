# M5B-LSP-006: bounded declaration completion

Status: implemented as a prototype slice.

## Decision

Expose `textDocument/completion` in the stdio LSP service for the current
single-document snapshot. The query maps the requested UTF-16 position to a
source byte, extracts the ASCII identifier prefix immediately before it, and
filters declaration records whose names begin with that prefix.

The response is a deterministic LSP `CompletionList` with `isIncomplete: false`.
Each item includes a completion kind, shared resolved-signature detail where
available, and a `textEdit` replacing the current prefix. Invalid positions,
unknown documents, and incomplete documents return an empty list.

## Migration and compatibility

Completion consumes `DeclarationIndex` declaration records and resolved
signatures; it does not parse names, infer types, or construct a second symbol
table. The first boundary intentionally returns current-document declaration
candidates rather than claiming scope-aware workspace completion. The
single-document stdin boundary and stdin import rejection remain unchanged.

## Quantitative gate

`tests/lsp_tests.py` verifies the completion capability, filters the `ad`
prefix to the `add` function, checks its signature detail, and checks the
UTF-16/source replacement range. The test remains registered as the
`language_server` CTest and M0A inventory case.

## Out of scope

- scope-aware ranking and shadowing-aware candidate filtering;
- imported, cross-file, or workspace-wide completion;
- completion resolve requests, snippets, and commit characters;
- persistent semantic caches and range-based synchronization.

## Old-path deletion condition

No editor-specific symbol table is introduced. Future completion work must
continue to consume shared declaration metadata and module interfaces before
the broader M5B workspace boundary is complete.
