# M5B-LSP-009: namespace-alias member definition

Status: implemented as a prototype slice.

## Decision

Extend cross-module `textDocument/definition` to namespace-qualified member
uses such as `import "./lib.cd" as lib; print lib.value;`. The query keeps the
existing local declaration lookup, identifies the field token on a
namespace-alias `FieldAccessExpr`, and follows that alias import through the
target module's explicit export or export-from chain.

The response remains a normal LSP location containing the target opened URI
and source-local declaration range. Namespace alias declarations themselves
continue to use the local declaration path; ordinary struct field accesses do
not become cross-module definitions merely because they share the same AST
shape.

## Migration and compatibility

The production reference-site collector records field-access ranges alongside
existing variable/assignment sites. The shared virtual workspace snapshot and
export-chain lookup from M5B-LSP-008 remain the only semantic services used;
the adapter does not synthesize a declaration ID for an imported interface
binding. Existing references, hover, completion, rename, document symbols,
and workspace symbols remain source-local.

## Quantitative gate

`tests/lsp_tests.py` verifies both an unqualified direct import and a
namespace-alias member definition against the same opened module, including
the target URI and declaration range. The test remains registered as the
`language_server` CTest and M0A inventory case.

## Out of scope

- namespace-qualified type and enum-variant navigation;
- cross-module references, rename, and completion;
- filesystem-backed dependency discovery outside the open URI set;
- persistent semantic caching and range-based synchronization.

## Old-path deletion condition

No namespace-specific resolver is added. Namespace-member navigation continues
to adapt the shared module graph and export metadata until cross-module
semantic identities are directly available to all LSP queries.
