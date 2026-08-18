# Block and Documentation Comments

Status: implemented on 2026-08-18.

## Decision

Source comments support the existing `//` line form and a non-nested
`/* ... */` block form that may span lines. The lexer skips both forms, and an
unterminated block comment is a lexical diagnostic at its opening location.
`///` remains ordinary line-comment text; this slice does not add a
documentation AST or documentation extraction semantics.

## Compatibility

Comments remain outside the parser AST, type checking, IR, bytecode, module
interfaces, and VM behavior. Lossless source views classify block comments as
trivia with source-local ranges, and the formatter copies their bytes while
normalizing surrounding whitespace. Block comments are not nested.

## Verification

`tests/golden/block_comments` covers multi-line, inline, trailing, and `///`
comments through formatting, AST, bytecode, and Rust VM execution.
`block_comment_unterminated` locks the lexer diagnostic and exit code.
