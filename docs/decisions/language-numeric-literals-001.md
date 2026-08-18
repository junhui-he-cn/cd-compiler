# Numeric Literal Separators and Exponents

Status: implemented on 2026-08-18.

## Decision

Keep `number` as the only numeric source type and continue lowering it to the
existing `f64` runtime value. Integer, fractional, and exponent digit runs may
contain one underscore only between adjacent digits. Decimal exponents use
`e` or `E` and may have an optional sign.

Accepted examples include `1_000_000`, `3.141_592`, `1e10`, `1e-6`, and
`2.5E10`. Leading, trailing, or repeated separators and incomplete exponents
are lexer errors with source locations. Hexadecimal, binary, octal, suffixes,
and additional integer types remain out of scope.

## Compatibility

The token lexeme remains lossless, so AST output and formatting retain the
source spelling. IR compilation removes separators before parsing the value;
bytecode, module interfaces, and the Rust VM continue to use the existing
numeric representation and do not change their contracts.

## Verification

`tests/golden/numeric_literals` covers separator and exponent forms through
AST, IR, bytecode, and Rust VM output. The `numeric_invalid_forms` parse-error
fixture covers trailing/repeated separators and incomplete exponents. The
formatter unit and corpus gates verify that accepted source spelling remains
stable.
