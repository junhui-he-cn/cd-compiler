# M5B-LSP-006: bounded completion

Status: implemented as a prototype slice.

## Decision

Expose `textDocument/completion` in the stdio LSP service for the current
single-document snapshot. The query maps the requested UTF-16 position to a
source byte, extracts the ASCII identifier prefix immediately before it, and
filters declaration records whose names begin with that prefix. It also offers
the language keyword queue so a newly typed statement can be completed. When
the current text is temporarily incomplete and no AST is available, the server
returns the same prefix-filtered keyword queue instead of suppressing
completion. The server advertises `.` as a trigger character for automatic
member and namespace completion; ordinary identifier completion remains
available through the editor's explicit completion command or quick suggestions.

The response is a deterministic LSP `CompletionList` with `isIncomplete: false`.
Each item includes a completion kind, shared resolved-signature detail where
available, and a `textEdit` replacing the current prefix. Invalid positions and
unknown documents return an empty list; incomplete documents retain keyword
completion even when semantic candidates are unavailable.

## Migration and compatibility

Completion consumes `DeclarationIndex` declaration records and resolved
signatures; it does not parse names, infer types, or construct a second symbol
table. The first boundary intentionally returns current-document declaration
candidates plus language keywords rather than claiming scope-aware workspace
completion. The single-document stdin boundary and stdin import rejection
remain unchanged.

## Quantitative gate

`tests/lsp_tests.py` verifies the completion capability and `.` trigger,
filters the `ad` prefix to the `add` function, checks the keyword queue on an
incomplete `pri` document, checks the `print` keyword and signature detail, and
checks the UTF-16/source replacement range. The test remains registered as the
`language_server` CTest and M0A inventory case.

## Out of scope

- scope-aware ranking and shadowing-aware candidate filtering;
- imported, cross-file, or workspace-wide completion;
- context-aware keyword filtering and native library names;
- completion resolve requests, snippets, and commit characters;
- persistent semantic caches and range-based synchronization.

## Old-path deletion condition

No editor-specific symbol table is introduced. Future completion work must
continue to consume shared declaration metadata and module interfaces before
the broader M5B workspace boundary is complete.
