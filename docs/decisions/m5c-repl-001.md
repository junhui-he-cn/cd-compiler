# M5C-REPL-001: source-backed incremental session

Status: implemented as a prototype slice.

## Decision

Add `tools/repl.py` as the first incremental evaluation boundary. The REPL
accepts blank-line-delimited source forms, `:reset`, `:help`, and `:quit`.
Each candidate form is appended to the accepted transcript, compiled by the
production C++ compiler with `--emit-bytecode`, and executed by the Rust VM.
The accepted transcript is the session state, so definitions and mutations
remain visible to later forms through deterministic replay.

Only stdout newly appended by a successful replay is emitted. A parse, type,
bytecode, or runtime failure emits its diagnostic to stderr and leaves both the
accepted transcript and the previously exposed output unchanged. `:reset`
clears the transcript and output baseline. Temporary source paths are rendered
as `<repl>` when the underlying diagnostic includes a path; pathless frontend
diagnostics retain the existing pathless format.

## Migration and compatibility

This slice wraps the existing production frontend, bytecode emitter, and Rust
VM; it does not introduce a parser, semantic checker, bytecode format, or
second runtime. The source-backed replay is intentionally a correctness-first
prototype. The session currently uses a temporary file and requires the
compiler binary plus `vm-rs/Cargo.toml`; a native in-process CLI protocol,
incremental VM state, and project-root import policy are later slices.

No expression-result echo is added: language programs use their existing
`print` statement for observable output. A form is submitted at a blank line,
which permits function and block forms to span multiple lines.

## Quantitative gate

`tests/repl_tests.py` verifies persistent definitions and assignment output,
compile-error rollback, `:reset` isolation, runtime-error rollback, temporary
path normalization, and a multi-line function form. It is registered as the
`repl` CTest and M0A inventory case.

## Out of scope

- in-process compiler/VM embedding;
- incremental bytecode or runtime-state reuse;
- automatic expression-result display;
- relative-import project roots, package manifests, and import maps;
- line-editing, history, completion, or terminal UI;
- persistent semantic caching.

## Old-path deletion condition

The replay wrapper can be replaced by a native session service only after an
equivalent session-state model preserves accepted definitions, output
ordering, diagnostic rollback, reset behavior, runtime-error behavior, and
the focused transcript corpus without replaying prior runtime side effects.
