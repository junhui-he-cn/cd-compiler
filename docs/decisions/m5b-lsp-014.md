# M5B-LSP-014: qualified enum-variant completion

Status: implemented as a prototype slice.

## Decision

Extend `textDocument/completion` for a receiver path that resolves to an
opened public enum, such as `api.Result.O`. The receiver path may use a direct
import, namespace alias, or export-from forwarding chain. Matching enum
variants are returned as CompletionItems with LSP EnumMember kind `20`,
`variant` detail, deterministic name/source ordering, and the existing UTF-16
prefix replacement edit.

Qualified struct paths resolve as types but do not expose ordinary struct
fields in this slice. Unopened or disk-only modules do not contribute
candidates.

## Migration and compatibility

Existing local declaration and namespace-alias value completion remains the
fallback. Qualified enum-path completion is selected only when the receiver
resolves to a struct or enum through the shared export resolver, preventing
ordinary value-member completion from changing behavior.

## Quantitative gate

`tests/lsp_tests.py` verifies completion for the `O` prefix in re-exported
`api.Result.Ok`, including item kind, detail, and exact replacement range. The
test remains registered as the `language_server` CTest and M0A inventory case.

## Out of scope

- ordinary struct-field completion;
- completion from unopened dependencies;
- type/variant references or rename edits;
- persistent semantic caching and range-based synchronization.

## Old-path deletion condition

The completion candidate adapter can move into shared semantic completion
metadata once enum variants have first-class exported identities.
