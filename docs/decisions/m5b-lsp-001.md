# M5B-LSP-001: first stdio language-server boundary

Status: implemented as a prototype slice.

## Decision

Expose `compiler_design --lsp` as a stdio JSON-RPC service. The service
supports `initialize`, `shutdown`, and `exit`, full-document
`textDocument/didOpen`, `textDocument/didChange`, and `textDocument/didClose`,
`textDocument/publishDiagnostics`, and `textDocument/formatting`.

Document changes are kept in memory and rechecked from the complete current
text. Diagnostics are converted from the existing `FileDiagnosticError` and
`TypeErrorList` records, including source ranges where the production front
end provides them. Formatting reparses through `FrontendSession`, consumes the
production `LosslessSourceView`, and returns one whole-document LSP edit from
`formatLosslessSource()`.

The initial service is deliberately single-document in product scope and uses
the existing stdin front-end boundary. Consequently, imports retain the
existing stdin rejection diagnostic. The protocol adapter does not implement a
second lexer, parser, type checker, name resolver, or formatter.

## Migration and compatibility

The CLI remains unchanged outside the additive `--lsp` mode. The LSP uses the
same front-end, diagnostic, and formatter services as the CLI; repeated edits
rebuild only the current in-memory document because multi-file/module caching
is outside this prototype. JSON-RPC messages use standard `Content-Length`
framing and UTF-8 JSON; LSP positions use zero-based lines and UTF-16 code
units.

## Quantitative gate

`tests/lsp_tests.py` covers capability negotiation, clean diagnostics, a
formatting edit, parse diagnostics, type diagnostics, close cleanup, and
shutdown/exit. It is registered as the `language_server` CTest and in the M0A
verification inventory. This slice does not claim the broader M5B symbol,
cross-module, or unchanged-module cache gate.

## Out of scope

- symbol lookup, definitions, references, hover/type information, completion,
  and module navigation;
- incremental range synchronization and multi-file workspace state;
- editor-specific partial-document recovery;
- persistent semantic or module caches.

## Old-path deletion condition

There is no duplicate semantic path to delete. Future LSP queries must consume
snapshot-stable semantic IDs, source ranges, and module interfaces before the
broader M5B editor adapter is considered complete.
