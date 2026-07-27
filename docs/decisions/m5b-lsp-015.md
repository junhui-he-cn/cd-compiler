# M5B-LSP-015: typed struct-field completion

Status: implemented as a prototype slice.

## Decision

Extend `textDocument/completion` for a field prefix on a `FieldAccessExpr`
whose receiver has a known named struct `TypeInfo`. Resolve the struct name
through the current module, opened direct imports, namespace aliases, and
export-from chains, then return the source-declared fields with LSP Field kind
`5`, `field` detail, deterministic source ordering, and the existing UTF-16
prefix replacement edit.

Nullable wrappers are unwrapped for candidate discovery when the production
type record already identifies the struct. Enum values, namespace value
members, unknown/dynamic receivers, and unopened modules do not contribute
fields.

## Migration and compatibility

Qualified enum-path completion and opened namespace value completion retain
priority for their own receiver paths. Struct-field completion is selected
only when a typed field-access site identifies a named struct, so unknown
receivers continue through the bounded local completion fallback.

## Quantitative gate

`tests/lsp_tests.py` verifies the `va` prefix in `print box.value` where `box`
has the re-exported `api.Box` type, including item kind, detail, and exact
replacement range. The test remains registered as the `language_server` CTest
and M0A inventory case.

## Out of scope

- fields for unknown or dynamic receivers;
- completion from unopened dependencies;
- method completion and field documentation;
- persistent semantic caching and range-based synchronization.

## Old-path deletion condition

The source AST field adapter can move into shared semantic shape metadata once
field declarations and type substitutions have first-class completion records.
