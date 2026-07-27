# M5C-REPL-004: JSON Lines session protocol

Status: implemented as a prototype slice.

## Decision

Add an optional `--json-lines` mode to `tools/repl.py` for machine clients.
The mode reads one JSON object per input line and writes one JSON response per
request line. A request contains either a `source` string or a `command` with
one of `reset`, `help`, or `quit`.

Successful source submissions preserve the existing source-backed replay
boundary and return the newly produced stdout suffix in `stdout`. Failed
parse, type, bytecode, runtime, or protocol requests return `ok: false`, an
empty `stdout`, and an `error` string. Candidate source remains uncommitted on
compiler and runtime failures. `reset` clears accepted source and the output
baseline; `help` returns the existing help text in the response; `quit` returns
a final success response and ends the session.

The session converts all responses to JSON before writing them, so compiler and
VM stderr cannot corrupt the line protocol. Existing diagnostic normalization
continues to replace temporary session paths with `<repl>` or
`<repl-root>`.

## Migration and compatibility

The protocol is an adapter over the existing production compiler and Rust VM;
it introduces no parser, type checker, bytecode format, or runtime state
implementation. Interactive blank-line submission remains unchanged, and
`--import-path` and `--session-root` continue to use the production import
resolution behavior.

## Quantitative gate

`tests/repl_tests.py` submits JSON source requests across declaration,
stdout-suffix, compiler-error rollback, reset, and quit transitions. A second
transcript covers runtime-error rollback and verifies that JSON mode writes no
raw stderr.

## Out of scope

- JSON-RPC framing or request identifiers;
- in-process compiler or VM state;
- incremental bytecode or runtime-state reuse;
- expression-result echo;
- terminal history, completion, or line editing.

## Old-path deletion condition

The JSON Lines adapter can move into a native session service once that service
preserves the same accepted-transcript, output-suffix, import, diagnostic, and
rollback corpus without duplicating frontend or runtime semantics.
