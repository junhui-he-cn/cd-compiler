# Uniform Trailing Commas

Status: implemented on 2026-08-18.

## Decision

Comma-delimited source lists accept one optional trailing comma. The rule
covers declaration and parameter lists, type parameters and arguments, call
arguments, array/map/struct literal entries, import/export names, and variant
or record pattern arguments. Existing enum variant and `match` arm behavior is
kept unchanged.

An empty list remains valid only where it was already valid. A leading comma,
two consecutive commas, and a comma followed by another element delimiter
remain parse errors. A trailing comma is syntax only; it does not create an
AST element or affect type checking, IR, bytecode, module interfaces, or VM
behavior.

## Compatibility

This is an additive source syntax change. Existing programs and generated
artifacts are unchanged. The lossless formatter preserves a trailing comma
that was present in the source and does not insert one into sources that omit
it.

## Verification

The `trailing_commas` golden fixture covers generic declarations and arguments,
enum payloads, struct fields and constructors, function and call parameters,
array/map literals, and record/variant patterns. Formatter coverage also
includes selective import and export name lists.
