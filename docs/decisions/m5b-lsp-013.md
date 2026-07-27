# M5B-LSP-013: namespace-qualified type and enum-variant navigation

Status: implemented as a prototype slice.

## Decision

Extend `textDocument/definition` to type and enum-variant references in the
currently opened virtual workspace. Qualified type annotations and qualified
struct constructors resolve a local struct/enum or an exported type reached
through a namespace alias or direct import. Enum constructors and match
patterns resolve their variant token through the corresponding enum target.
Explicit export and export-from chains are followed to the source declaration;
variant results use the source enum's variant name range.

The adapter records source-ranged type/variant sites while traversing the
production AST and reuses the existing module graph, declaration index, and
export-chain resolver. Snapshot-local IDs are not serialized, and ordinary
struct fields keep their existing value-member behavior.

## Migration and compatibility

Existing value definition lookup runs first. Type navigation is a fallback for
qualified annotations, constructors, and patterns, so current local/imported
value and namespace-member results remain unchanged. Unopened or disk-only
dependencies do not contribute targets.

## Quantitative gate

`tests/lsp_tests.py` opens a module and an export-from forwarding module, then
verifies `api.Box` resolves to the source struct declaration and
`api.Result.Ok` resolves to the source enum variant range. The test remains
registered as the `language_server` CTest and M0A inventory case.

## Out of scope

- type and enum-variant references or rename edits;
- ordinary struct-field navigation and completion;
- unopened dependencies and persistent semantic caching;
- range-based synchronization.

## Old-path deletion condition

The AST site adapter can move into shared semantic reference metadata once
type and variant references have first-class snapshot identities and export
links.
