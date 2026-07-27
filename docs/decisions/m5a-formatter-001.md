# M5A-FORMAT-001: production lossless formatter baseline

Status: implemented as the first M5A formatter slice. `M5A-FORMAT-002` adds
the corpus gate and `M5A-FORMAT-003` adds the CLI exposure of the existing
indentation option.

## Decision

Expose a `--format` mode and a reusable `formatLosslessSource()` service. The
service consumes `LosslessSourceFileView` pieces built from the production
lexer tokens and original source bytes; it does not tokenize, parse, or
reconstruct comments independently.

The first stable layout uses two-space indentation, puts non-empty braces on
their own multiline layout, keeps arrays and delimited call/type lists inline,
and normalizes token spacing around operators, commas, colons, and statement
boundaries. String lexemes and line-comment text are copied byte-for-byte.
Inline comments remain inline when the source placed them on the same line as
the preceding item; comments that began on a later source line remain leading
comments. Formatted output ends with one newline when it is non-empty.

`--format` performs the normal production lex/parse step and therefore rejects
invalid or incomplete input with the existing diagnostics. It does not run
type checking or backend lowering. For an import graph, dependencies are
loaded and parsed for the same syntax boundary, but only directly requested
entry source files are emitted. Direct multi-file inputs are emitted in their
source order with one blank separator.

## Migration and compatibility

Existing AST, semantic, IR, bytecode, and runtime modes remain unchanged. The
formatter is an additive consumer of the existing lossless view. Formatting is
validated by reparsing the result and comparing AST output for representative
syntax; later M5A slices may improve layout without adding a second grammar.

## Quantitative gate

- the C++ formatter suite proves lossless reconstruction, comments, generic
  calls, for-header semicolons, idempotence, and AST equivalence;
- the CLI suite covers stdin, import entries, invalid input, and mode conflicts;
- every named formatter case round-trips through `format(format(source))`;
- the canonical verification inventory includes both formatter suites.

## Old-path deletion condition

No production path is deleted here. Any future formatter-specific tokenization,
trivia recovery, or parser code remains prohibited; it can be removed only
after all formatter inputs use the production lossless view and the M5A corpus
passes comment, range, parse, semantic, and idempotence gates.

## Explicitly deferred

Invalid/incomplete-input formatting, configuration files, line-width wrapping,
doc comments, LSP formatting requests, and a complete error-fixture formatter
corpus remain later M5A slices.
