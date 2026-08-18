# C-Style For Compatibility Policy

Status: accepted on 2026-08-18.

## Decision

The C-style `for` statement remains a supported legacy form for source
compatibility. New code should prefer `for-in` over arrays, maps, or ranges.
The initializer, condition, and increment clauses retain their current
optional forms, and `continue` continues to run the increment clause before
the next condition check.

This decision marks the syntax as legacy in documentation only. It does not
remove the grammar, change scope or control-flow semantics, or introduce a
new replacement syntax.

## Diagnostics and compatibility

The compiler has no warning or deprecation diagnostic channel. C-style `for`
therefore does not emit an implicit warning, and existing stdout, stderr,
exit-code, AST, IR, bytecode, `.cdbc 0.2`, and Rust VM behavior remain
unchanged. A future warning or removal requires a separate decision covering
diagnostic policy, migration guidance, and compatibility timing.

## Verification

The existing C-style `for` fixtures cover counting, omitted initializer,
condition, and increment clauses, `break`, `continue` increment ordering,
scope escape, and parse/type errors. The existing `for-in` and range fixtures
cover the recommended iteration forms. This slice adds no runtime behavior or
fixture output changes.
