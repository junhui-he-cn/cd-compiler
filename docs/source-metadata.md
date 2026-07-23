# Source metadata

M1A1 adds source metadata beside the existing compiler representations.

- `SourceFileId`, `SyntaxNodeId`, `DeclarationId`, `SymbolId`, `BindingId`, and
  `ScopeId` are domain-typed, snapshot-local IDs. Their numeric values are only
  meaningful during one front-end/type-checker snapshot and are never artifact,
  cache, or cross-build keys.
- `SourceRange` is a source-local half-open byte interval `[start, end)` with a
  `SourceFileId`. `Token`, AST nodes, bindings, and located diagnostics retain
  these ranges while the existing `SourceSpan` and string resolved names stay
  available for compatibility.
- `sourcePositionAt` converts a byte offset to the existing 1-based line and
  column coordinates. The end offset is allowed to equal the source length, so
  an end position can identify the point immediately after the final byte.
- Direct multi-file inputs keep each token and diagnostic range local to its
  original file even though the parser still consumes the combined source.
  User-facing diagnostics continue to use the established path/line/column
  format.

The current proof slice covers lexical declarations, block scopes, variable
reads, assignments, and direct multi-file diagnostics. Bytecode emission does
not serialize these snapshot-local identities; artifact-local debug identity
remains owned by M4A/M4B.

## Declaration index (M1B initial slice)

`DeclarationIndex` is a snapshot-local AST index built beside the existing
`TypeChecker` path. It records declaration IDs and symbols for lexical values,
parameters, structs, enums, methods, and namespace aliases; scope parents and
lexical lookup tables; function/method signatures; import/export metadata; and
the targets of variable reads, ordinary assignments, and compound assignments.
Direct calls through lexical value bindings receive `CallTargetRecord` values,
and locally declared struct method calls receive exact method declaration and
symbol targets after the shadow comparison. For-in variables and match pattern
variables expose the same declaration/symbol target shape, including one shared
target for all occurrences of an OR-pattern binding.
Struct and enum declarations retain their AST records for field and variant
metadata, while signatures retain their type parameters, parameters, and
optional return annotations.
`DeclarationIndex::signature()` and `DeclarationIndex::shape()` provide
DeclarationId-based queries for these records without resolving annotations to
canonical semantic types; that ownership remains with the later type model.

The checker exposes the index and a shadow-comparison count. During this
migration slice, type and namespace qualifiers are not treated as value reads,
and OR-pattern occurrences share one declaration record. Native calls, enum
constructors, namespace-qualified calls, and imported methods remain external
targets in this slice; only locally available function bindings and method
declarations are materialized. The old `ResolvedNames` implementation remains
the behavior oracle; module graph resolution and materialization of imported
value symbols are deferred to M3A. The index IDs are not serialized into
`.cdbc` artifacts or used as cache keys.

## Typed expression metadata (M1C slices)

`DeclarationIndex::typedExpression()` exposes the `TypeInfo` produced by the
existing checker for resolved variable reads, ordinary assignments, numeric
compound assignments, direct calls, field access, field assignments and field
compound assignments, and index reads/assignments/compound assignments, native
function/member calls, array/map literals, and named struct constructors. The
records include both statically known and dynamically validated index paths plus
dynamic collection inference. Native call records retain the resolved native
name; legacy `len` lowering remains outside this registry. All records are keyed
by the AST expression address within the current snapshot, are not persistent
identities, and do not claim canonical type ownership. The checker requires these
records during shadow comparison for the migrated expression families. IR
lowering consumes native-call records for direct and member calls, array/map/
struct type records, and field-assignment result records; `len`, collection
helpers, and other unmigrated families remain on the legacy AST/`ResolvedNames`
path.

## Lossless source view

`FrontendSession::losslessSourceView()` groups `LosslessPiece` values by
`SourceFileId`. Existing lexer tokens become token pieces; every byte gap
between adjacent token ranges becomes one or more trivia pieces. Line comments
are classified from those already-known gaps, while their bytes and all other
trivia are copied directly from `SourceFile::text`. No second lexer or parser
grammar is involved. Concatenating a file view's pieces reproduces the exact
original source bytes, including comments, whitespace, and comment placement.
