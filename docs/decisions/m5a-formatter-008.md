# M5A-FORMAT-008: incomplete-input policy

Status: implemented.

Reject invalid and incomplete input through the production lexer/parser before
the formatter emits any output. `--format` and `--format-check` retain their
existing exit status and diagnostic text, and neither mode returns a partially
formatted prefix. This covers unterminated delimiters, unfinished statements,
and unterminated strings; recovery remains owned by the normal front-end
diagnostic behavior.

The formatter therefore remains a complete-source tool. Editor-specific
partial-document formatting and recovery are out of scope for this slice and
would require a separate incremental syntax contract.

Focused CLI tests cover incomplete arrays, blocks, and strings for both format
modes; the existing formatter C++ suite and 235-case corpus remain required
gates.
