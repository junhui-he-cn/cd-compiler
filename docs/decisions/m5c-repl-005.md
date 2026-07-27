# M5C-REPL-005: explicit expression-result evaluation

Status: implemented as a prototype slice.

## Decision

Add an explicit expression-result boundary to `tools/repl.py`. Interactive
clients use `:eval EXPR`; JSON Lines clients use a request such as
`{"expression":"value * 2"}`. An optional trailing semicolon is accepted for
the interactive command.

The wrapper evaluates the expression by appending `print (EXPR);` to the
accepted source transcript and replaying it through the production compiler
and Rust VM. This preserves the language's existing value formatting and
newline behavior. Successful expressions expose only their new stdout suffix;
assignment and other expression side effects remain in the accepted
transcript. Parse, type, bytecode, and runtime failures do not commit the
expression form.

The explicit request shape avoids adding a second Python parser or guessing
whether an arbitrary multi-line form is an expression, declaration, or
statement. Ordinary source submissions retain their existing no-echo behavior.

## Migration and compatibility

The feature is an adapter over the existing production `print` statement and
source-backed replay boundary. It does not add a language construct, evaluator,
bytecode opcode, or alternate value formatter. Imports, session roots, reset,
JSON response framing, diagnostic normalization, and failure rollback remain
unchanged.

## Quantitative gate

`tests/repl_tests.py` covers interactive literal and variable expressions,
expression side-effect isolation from later output, failed-expression rollback,
and JSON expression requests alongside the existing source and runtime cases.

## Out of scope

- automatic echoing for every unmarked expression statement;
- in-process compiler or VM state;
- incremental bytecode or runtime-state reuse;
- terminal history, completion, or line editing.

## Old-path deletion condition

The source-level expression wrapper can move into a native session service once
that service can classify and evaluate expression requests through the same
frontend, runtime, output-suffix, and rollback corpus without duplicating
language semantics.
