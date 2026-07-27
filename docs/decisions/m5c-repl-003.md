# M5C-REPL-003: relative-import session root

Status: implemented as a prototype slice.

## Decision

Add `--session-root DIR` to `tools/repl.py`. When supplied, the wrapper creates
its temporary transcript source inside that directory, so existing explicit
relative imports resolve through the production `FrontendSession` relative to
the selected project root. The temporary source is removed when the session
ends; the replay artifact remains in the wrapper's private temporary work
directory.

Import search paths from M5C-REPL-002 remain repeatable and ordered. The
accepted transcript, output-suffix, reset, and failed-form rollback rules are
unchanged. Session diagnostics replace the temporary transcript/project paths
with `<repl>` or `<repl-root>` while preserving the existing diagnostic kind,
line, column, and source text.

## Migration and compatibility

Relative import resolution is still owned by `FrontendSession`; the REPL adds
only a source-file placement option. No package manifest, import map, or
duplicate resolver is introduced. Without `--session-root`, the temporary
source directory remains the relative-import base, preserving the earlier
prototype behavior.

## Quantitative gate

`tests/repl_tests.py` creates a temporary project root and `lib.cd`, imports it
with `import "./lib.cd";`, and verifies the exported value is available to a
later accepted form without leaking the temporary path.

## Out of scope

- package manifests and import maps;
- persistent project/session roots across process restarts;
- in-process compiler/VM state;
- incremental bytecode or runtime-state reuse;
- terminal history, completion, or line editing.

## Old-path deletion condition

The temporary-source placement adapter can move into a native session service
once the service exposes the same project-root identity, relative/import-path
ordering, diagnostic normalization, and rollback behavior without a duplicate
module resolver.
