# M5B-LSP-018: closed-module workspace navigation

Status: implemented for cross-module definition and references.

## Decision

The language server accepts file-backed workspace roots from
`initialize.params.workspaceFolders` in the declared order. When that array is
missing or empty, `initialize.params.rootUri` supplies one root. Non-file URIs
are ignored. Roots are canonicalized and de-duplicated before they reach the
production `FrontendSession`.

Opened documents remain virtual sources and take precedence over disk text at
the same canonical path. An imported disk module is eligible only when its
existing canonical path is inside one of the declared roots. Relative imports
try the importing module's directory first; non-explicit imports then use the
declared roots in order and retain the existing `.cd` suffix fallback. With no
declared root, the virtual workspace does not read imported disk files.

The snapshot keeps the existing source-local IDs internally and assigns every
closed disk source a canonical percent-encoded `file://` URI. `definition` and
`references` use the complete production module graph, so a location may point
to a closed module. Missing imports and imports outside the roots remain
diagnostics on the importing open document; no outside file is read.

## Migration and compatibility

The slice extends `FrontendSession` with a virtual-workspace disk-import
boundary and reuses `ModuleGraph`, `DeclarationIndex`, and the existing export
resolver. It does not add an LSP parser, resolver, or persistent semantic
cache. Existing opened-document precedence, direct multi-file CLI behavior,
artifact formats, and VM behavior are unchanged.

Completion, rename edits, workspace symbols, hover, cache persistence,
workspace file watching, and range-based synchronization do not gain closed
module support in this slice. They remain on the opened-document boundary.

## Quantitative gate

`tests/lsp_tests.py` uses a temporary workspace to cover a direct import from a
closed disk module, definition and references with and without the declaration,
open-document-over-disk precedence, direct and nested missing imports, and an
import that would escape the workspace root. The existing `language_server`
CTest and M0A inventory case remain the protocol gate.

## Old-path deletion condition

The opened-only virtual-source restriction can be removed only after every
consumer that needs closed modules declares its own workspace-root and source
precedence contract. This slice does not authorize general disk indexing or
persistent semantic state.
