# M5A-FORMAT-005: top-level blank-line policy

Status: implemented.

Retain at most one blank line between top-level syntax items when the source
gap contains two or more line breaks. Leading and trailing blank lines are
removed. Blank lines inside braces, brackets, parentheses, or generic angle
delimiters continue to use the existing canonical layout; this slice does not
introduce line wrapping or trailing-comma normalization. A top-level blank gap adjacent to a
line comment is retained, while the comment bytes and source order remain
unchanged.

The implementation records the policy from whitespace already present in the
production `LosslessSourceFileView`. It does not add a tokenizer, parser, or
semantic/backend stage. Focused C++ and CLI tests cover leading/trailing
normalization, multiple source blank lines, comments, nested blank lines,
idempotence, and AST parity; the existing 235-case formatter corpus remains a
required gate.

Trailing-comma preservation is resolved separately by M5A-FORMAT-006. Line-width
wrapping and incomplete-input formatting remain separate future decisions.
