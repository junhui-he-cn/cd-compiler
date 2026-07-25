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
lowering consumes native-call records for direct and member calls, typed call
result records, local direct/member-call targets, and variant-constructor
records, variable, assignment, and compound-assignment result records,
array/map/struct type records, field-access and field-assignment result records,
and index result records; `len`, collection helpers, and other unmigrated
families remain on the legacy AST/`ResolvedNames` path.

## Function and return metadata (M1D initial slice)

`DeclarationIndex` retains declaration/signature records for named functions
and methods, records typed function-expression results, and records the checked
type of every return statement. The initial M1D lowering slice requires these
records for named functions, methods, anonymous functions, and returns while
preserving the existing runtime names and closure-cell representation.

The M1D capture-set slice adds one `CaptureRecord` for every named function,
method, and function expression. Its `ResolvedSymbol` entries are deduplicated
by declaration ID in source traversal order and cover variable reads,
assignments, and compound assignments that cross a nested function boundary
to an enclosing function-owned local cell. Top-level/module bindings remain
outside the capture set because the current runtime resolves them through its
global environment. IR lowering requires the record's presence but continues
to emit the existing variable operations and function values, so capture
metadata is not serialized into `.cdbc` artifacts.

The loop-control slice adds `LoopTargetRecord` entries for every valid
`break` and `continue`. Each record identifies the nearest `while`, C-style
`for`, or `for-in` statement and its loop kind. The collector clears the active
loop stack at function and method boundaries, so nested functions cannot target
an enclosing loop. IR lowering checks the semantic target against its active
loop context before emitting the existing jump or patched-break instructions;
the records remain snapshot-local and are not serialized into `.cdbc`.

## Resolved declaration signatures (M1E1 initial slice)

`DeclarationIndex::resolvedSignature()` exposes a canonical `TypeInfo` function
type for every checked named function and method. Named function signatures
include resolved parameter and return types, generic parameter names, and
concrete type-parameter constraints. Method signatures include the implicit
receiver as their first parameter and preserve method generic constraints;
receiver generic parameters remain represented inside the resolved receiver
type. The existing `signature()` query remains the AST-backed annotation view
for declaration-collection compatibility, while IR lowering now requires the
resolved signature rather than using the raw annotation record. These type
records are snapshot-local and are not serialized into `.cdbc` artifacts.

## Shared semantic type utilities (M1E1 migration slice)

`TypeUtils` now exposes the shared `SemanticTypes` interface for the type
operations used by the checker. `TypeSubstitutions` is the common mapping from
generic parameter names to resolved `TypeInfo` values. The interface currently
owns known-type, nullable, and function-signature identity predicates;
assignment/function compatibility; nullable and nested aggregate compatibility;
array/map element-type merging; and recursive substitution
through arrays, maps, nullable types, function signatures, nominal aggregate
arguments, constraints, and generic constraints.
`SemanticTypes::inferTypeArguments()` recursively matches the same aggregate,
nullable, and function shapes and returns a snapshot-local
`TypeInferenceConflict` instead of formatting diagnostics. `TypeChecker`
retains the diagnostic adapter and constraint-context handling, so inference
errors keep their existing source token and message shape.
`SemanticTypes::validateTypeParameterConstraints()` likewise returns a
`TypeConstraintViolation` for the first mismatched resolved argument without
owning context or diagnostic formatting; the checker remains responsible for
the existing `context: type parameter ... must satisfy ...` message.

`TypeChecker` routes its compatibility checks, collection inference, generic
instantiation, callback specialization, struct/enum payload checking, and
pattern binding checks through `SemanticTypes`. The old unqualified helpers
remain forwarding APIs for source compatibility during the migration; they do
not hold a second implementation. This slice preserves diagnostics, IR,
`.cdbc` artifacts, runtime behavior, and snapshot-local type records.

## Collection index operations (M1E2 initial slice)

`DeclarationIndex::indexOperation()` exposes one `IndexOperationRecord` for
each checked array/map/range or dynamic index read, index assignment, and
numeric index compound assignment. The record carries the operation kind plus
the resolved collection, index, and expression-result `TypeInfo`; unknown
types remain explicit for dynamic runtime-validation paths. `IRCompiler`
requires the matching record kind before emitting the existing `Index`,
`AssignIndex`, or compound-assignment sequence, so it no longer uses a generic
typed-expression record as the operation decision. The record is
snapshot-local, and no IR opcode or `.cdbc` format changes are involved.

`DeclarationIndex::fieldOperation()` records struct and dynamic field reads,
assignments, and numeric compound assignments with the resolved field name,
field type, and result type. `DeclarationIndex::structConstructor()` records
the resolved struct type and source field order for named constructors.
`IRCompiler` consumes these records for field operations and constructor field
names; namespace value accesses still use their existing resolved-name target
after the operation record is validated.
`VariantConstructorRecord` additionally carries the resolved enum result type
and substituted payload types, so variant lowering validates constructor shape
without reconstructing generic payload types from the AST.

## Literal pattern inputs (M1E3 initial slice)

`DeclarationIndex::literalPattern()` exposes the checked literal text and
`TypeInfo` for every `nil`, boolean, number, or string pattern, including
nested patterns in match arms. `IRCompiler` consumes the record to create the
existing comparison constant; coverage and diagnostic decisions remain in
`TypeChecker`, and no pattern-specific opcode or artifact format changes.

## Variant pattern inputs (M1E3 next slice)

`DeclarationIndex::variantPattern()` exposes the checked runtime enum and
variant identity, the matched non-nullable enum type, source-order payload
types after generic substitution, and the declared payload indices selected by
positional or named patterns. `IRCompiler` consumes those fields for existing
variant-tag and variant-field lowering; nested patterns, coverage, and
diagnostic decisions remain owned by `TypeChecker`.

## Lossless source view

`FrontendSession::losslessSourceView()` groups `LosslessPiece` values by
`SourceFileId`. Existing lexer tokens become token pieces; every byte gap
between adjacent token ranges becomes one or more trivia pieces. Line comments
are classified from those already-known gaps, while their bytes and all other
trivia are copied directly from `SourceFile::text`. No second lexer or parser
grammar is involved. Concatenating a file view's pieces reproduces the exact
original source bytes, including comments, whitespace, and comment placement.
