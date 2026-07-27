# M5B-LSP-012: opened-module completion

Status: implemented as a prototype slice.

## Decision

Extend `textDocument/completion` with public names from modules currently
opened in the virtual workspace. An unqualified prefix receives names from
direct imports in addition to current-source declarations. A prefix after an
opened namespace alias, such as `lib.va`, receives only the exported members
of that alias target and replaces the member prefix range.

Completion items reuse the target declaration kind and TypeChecker-produced
signature detail. Export and export-from chains are enumerated through the
same module graph and declaration target adapter used by definition,
references, and rename. The response remains a normal LSP `CompletionList`;
snapshot-local IDs are not serialized.

## Migration and compatibility

Local declaration filtering, prefix extraction, UTF-16 position mapping, and
deterministic item ordering remain the existing implementation. Imported
candidate collection is additive and does not read unopened files. Ordinary
struct field completion and namespace-qualified type/variant completion remain
outside this slice.

## Quantitative gate

`tests/lsp_tests.py` opens the target module and verifies completion for
`lib.va`, including the variable kind, detail, and exact member replacement
range. Existing local function completion remains covered by the same test.
The test remains registered as the `language_server` CTest and M0A inventory
case.

## Out of scope

- completion from unopened or disk-only dependencies;
- ordinary struct field completion;
- namespace-qualified type and enum-variant completion;
- persistent semantic caching and range-based synchronization.

## Old-path deletion condition

The local completion adapter remains the shared item serializer. Imported
candidate enumeration can move into the shared semantic completion service
once module export identities and scope candidates are first-class metadata.
