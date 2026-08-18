# Type-Position Optional Shorthand

Status: implemented on 2026-08-17 as the next language slice after the
module type-import alias delivery.

## Decision

Accept `T?` wherever a type annotation is parsed as shorthand for
`optional<T>`. The existing `optional<T>` spelling remains valid and is still
the canonical AST, interface, and diagnostic representation.

The shorthand applies recursively through arrays, maps, function return types,
generic arguments, and nested optional types. Expression-position `expr?`
remains the existing unwrap-and-early-return operator, while `??` remains the
coalescing operator.

## Compatibility

This is a source-level syntax addition only. It does not change `TypeInfo`,
runtime values, IR, bytecode, `.cdbc` artifacts, module cache keys, or VM
behavior. Existing `optional<T>` programs and serialized outputs remain
unchanged.

## Verification

The existing optional golden fixture exercises AST, IR, bytecode, module
interface, C++ execution, and Rust VM execution using the shorthand. Formatter
and LSP tests also parse the shorthand. The negative `number??` fixture keeps
the lexical boundary between type shorthand and expression coalescing
explicit.
