# M5A-FORMAT-006: trailing-comma policy

Status: implemented.

Preserve every trailing comma token accepted by the production parser, and do
not insert or remove trailing commas. Whitespace around the token still follows
the canonical formatter layout. This currently covers accepted trailing commas
in enum member lists and match-expression arms; syntax that the parser rejects
remains invalid formatter input and continues to use the normal diagnostics.

The policy follows the formatter's existing lossless-token contract: commas are
copied from the production `LosslessSourceFileView`, so this slice does not add
a tokenizer, parser, semantic stage, or backend behavior. Focused C++ and CLI
tests cover enum and match-expression trailing commas, idempotence, and AST
parity; the existing 235-case formatter corpus remains a required gate.

Line-width wrapping and incomplete-input formatting remain separate future
decisions.
