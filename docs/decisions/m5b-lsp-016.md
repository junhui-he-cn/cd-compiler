# M5B-LSP-016: typed struct-method completion

Status: implemented as a prototype slice.

## Decision

Extend `textDocument/completion` for a member-call prefix whose receiver has a
known named struct `TypeInfo`. Resolve the struct through local declarations,
opened direct imports, namespace aliases, and export-from chains, then return
method declarations for the resolved source struct. Method items reuse LSP
Method kind `2`, TypeChecker-resolved signature detail, deterministic source
ordering, and the existing UTF-16 prefix replacement edit.

Member-call reference sites are recorded separately from value field sites so
enum constructors and namespace value members retain their existing paths.
Unopened modules and unknown/dynamic receivers do not contribute methods.

## Migration and compatibility

Typed field completion runs first for ordinary `FieldAccessExpr` sites. Method
completion applies only to `MemberCallExpr` sites whose receiver type resolves
to a struct, while qualified enum completion remains responsible for enum
variant constructors.

## Quantitative gate

`tests/lsp_tests.py` verifies the `g` prefix in `print box.get()` where `box`
has the re-exported `api.Box` type, including Method kind, resolved signature
detail, and exact replacement range. The test remains registered as the
`language_server` CTest and M0A inventory case.

## Out of scope

- methods for unknown or dynamic receivers;
- completion from unopened dependencies;
- enum constructors and namespace value members as methods;
- persistent semantic caching and range-based synchronization.

## Old-path deletion condition

The source method adapter can move into shared semantic completion metadata
once exported method identities and receiver substitutions have first-class
records.
