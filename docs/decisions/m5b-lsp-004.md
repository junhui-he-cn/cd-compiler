# M5B-LSP-004: hover type information

Status: implemented as a prototype slice.

## Decision

Expose `textDocument/hover` in the stdio LSP service for the current
single-document snapshot. Hover uses UTF-16 position mapping and returns
plaintext type text with the selected source range.

Known expression types come from the shared `DeclarationIndex`
`TypedExpressionRecord` values. Callable declaration positions use the
TypeChecker-produced `ResolvedSignatureRecord` values. A query with no
available typed record, an invalid position, an unknown document, or an
incomplete document returns `null`.

## Migration and compatibility

The adapter reuses the document analysis, declaration index, source ranges, and
TypeChecker records introduced by the earlier LSP slices. It performs no
source-level type inference and does not serialize snapshot-local IDs. The
single-document stdin boundary and stdin import rejection remain unchanged.

## Quantitative gate

`tests/lsp_tests.py` verifies the hover capability, a callable declaration
signature (`fun(number): number`), and a variable reference type (`number`),
including their source ranges. The test remains registered as the
`language_server` CTest and M0A inventory case.

## Out of scope

- completion, signature help, and rename;
- synthesized types for every declaration form or untyped/incomplete AST node;
- imported or cross-module hover information;
- persistent semantic caches and range-based document synchronization.

## Old-path deletion condition

No editor-specific type inference is introduced. Future hover and completion
work must continue to consume shared semantic type records before the broader
M5B editor-service boundary is complete.
