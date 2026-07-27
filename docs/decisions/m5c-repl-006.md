# M5C-REPL-006: interactive terminal history

Status: implemented as a prototype slice.

## Decision

Enable Python readline editing and in-session history only for the ordinary
REPL path when both input and diagnostic streams are TTYs. Interactive lines
are read through `input()` while readline supplies cursor editing and up/down
history navigation. Non-empty source lines and commands are added to the
current readline history.

Add an optional `--history-file PATH` flag. When supplied in an interactive
readline session, the file is loaded at startup and written at session exit.
Without the flag, history remains in memory for the current process. The
accepted transcript, blank-line form boundaries, `:eval`, reset, output suffix,
and failure rollback rules remain unchanged.

JSON Lines and pipe-driven sessions do not load or write history and do not
receive terminal prompts. If readline is unavailable, the REPL falls back to
the existing stream loop without changing source evaluation semantics.

## Migration and compatibility

The feature is a terminal I/O adapter around the existing source-backed
session. It does not add a parser, evaluator, bytecode operation, or runtime
state implementation. The history file is optional and user-selected; no
default home-directory file is created.

## Quantitative gate

`tests/repl_tests.py` drives a real POSIX pseudo-terminal, submits a form and a
print form, replays the previous line with the up-arrow key, verifies the
replayed output, and checks that `--history-file` contains source lines and
commands. Existing pipe and JSON session tests continue to verify that no
prompts or history data leak into non-interactive output.

## Out of scope

- in-process compiler or VM state;
- automatic echoing for every unmarked expression statement;
- history search, completion, or custom key bindings;
- a default persistent history path.

## Old-path deletion condition

The terminal adapter can move into a native session service once that service
preserves the same TTY/non-TTY boundary, history-file lifecycle, transcript,
and rollback corpus without duplicating evaluation semantics.
