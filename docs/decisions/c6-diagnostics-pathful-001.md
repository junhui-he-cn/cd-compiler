# C6-DIAG-001: pathful file-backed diagnostics

Status: accepted on 2026-08-10; implemented in the diagnostics-pathful slice.

## Decision

All file-backed lexer, parser, and type diagnostics include the file path:

```text
<Kind> error at <path>:<line>:<column>: <message>
```

The path preserves the user's invocation spelling (a relative path stays
relative, an absolute path stays absolute). Only stdin keeps the pathless
form:

```text
<Kind> error at <line>:<column>: <message>
```

## Rationale

The previous "pathless for a single entry module without imports" rule was a
historical compatibility carve-out. It required per-file import knowledge at
diagnostic time, an error-path re-lex (`scanTokensUntil(Import)`), and a
parallel rule in `TypeChecker`. Every file is now an independent module, so a
single diagnostic format for file-backed modules removes the last
compatibility branch. LSP diagnostics are unaffected (they carry a separate
location and do not print the path).

## Migration

- `FrontendSession` no longer computes pathless diagnostics; `loadFile` lex and
  parse errors are always wrapped with the file context.
- `TypeChecker::pathlessModuleDiagnostics` was removed; module type errors are
  always wrapped with the file context.
- Stdin keeps pathless diagnostics through `parseSource(..., pathless=true)`;
  LSP single-document analysis is unchanged.
- Single-file parse/type error goldens were refreshed (379 `.err` files) by
  inserting the normalized `<repo>/...` path into the first diagnostic line;
  snippet/caret content and multi-line format were preserved.

## Gate

Full verification passed: CTest 46/46, golden 843/843, bytecode artifacts
126/126, module artifacts and cache 12/12, Rust VM goldens 802/802, canonical
inventory 1944/1944, boundary 5/5, malformed 108/108, and Cargo tests.
