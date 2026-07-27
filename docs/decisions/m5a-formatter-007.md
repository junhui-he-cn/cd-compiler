# M5A-FORMAT-007: bounded list wrapping

Status: implemented.

Use a fixed canonical width of 100 emitted source bytes. When a conservative
inline token-width estimate for a comma-separated array or parenthesized list
exceeds that width, emit
the opening delimiter, put the first item on the next line, put each later item
on its own continuation line, and place the closing delimiter at the base
indent. Continuation lines add exactly one formatter indentation level. This
also covers parenthesized call and declaration lists; a long parenthesized
expression without a comma is not split.

The formatter never splits a token, string, or line comment. A single item may
itself exceed 100 bytes; the list layout remains deterministic and the item
remains intact. Existing brace layout, top-level blank-line preservation, and
trailing-comma preservation remain unchanged. The CLI uses this fixed width in
both `--format` and `--format-check`; no new width option is introduced in this
slice.

Focused C++ and CLI tests cover long arrays, call arguments, non-list long
expressions, continuation indentation, idempotence, parseability, and AST
parity. The existing 235-case formatter corpus remains a required gate.

Incomplete-input formatting remains a separate future decision.
