# M5B-LSP-002: definition and document-symbol queries

Status: implemented as a prototype slice.

## Decision

Advertise and implement `textDocument/definition` and
`textDocument/documentSymbol` in the stdio LSP service.

`definition` accepts a zero-based LSP position, maps it to a source-local byte
range using UTF-16 code-unit positions, and resolves declarations through the
existing `DeclarationIndex`. It uses lexical reference records and the
TypeChecker's binding metadata for variable, assignment, and compound-
assignment occurrences. A resolved result is one LSP `Location` containing the
document URI and declaration range; an unavailable or invalid query returns
`null`.

`documentSymbol` returns a flat `DocumentSymbol[]` in deterministic source
order. Each item contains the declaration name, LSP symbol kind, declaration
range, selection range, and a short detail string. The query includes
parseable declarations in the current source snapshot and does not serialize
snapshot-local declaration or symbol IDs.

## Migration and compatibility

The document state now retains the parsed `Program`, a declaration index, and
reference sites after each full-document update. Type checking still uses the
production `TypeChecker`; the query adapter performs no name resolution or
type inference of its own. A document that parses but has type diagnostics can
still answer queries from its completed index; incomplete input has no query
snapshot. The single-document stdin boundary and stdin import rejection remain
unchanged.

## Quantitative gate

`tests/lsp_tests.py` verifies capability negotiation, deterministic symbol
names, function-call definition ranges, variable definition ranges, and the
existing diagnostics/formatting/shutdown flow. The test remains registered as
the `language_server` CTest and M0A inventory case.

## Out of scope

- references, rename, completion, hover/type information, and signature help;
- imported or cross-module definitions;
- persistent semantic caches and range-based document synchronization;
- exposing snapshot-local IDs as protocol fields.

## Old-path deletion condition

No duplicate symbol resolver is introduced. Future queries must continue to
consume the shared declaration/reference index and stable source ranges before
the broader M5B editor-service boundary is complete.
