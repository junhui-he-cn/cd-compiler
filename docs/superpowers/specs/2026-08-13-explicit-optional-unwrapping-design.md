# Explicit optional unwrapping design

Status: implemented on 2026-08-13.

## Problem

`optional<T>` narrowing is a compile-time flow analysis that proves a nullable
value is non-nil inside a branch. It supports direct variables, named-struct
fields, nested field paths, and array elements, plus a large invalidation
machinery for assignments, calls, callbacks, methods, closures, branch joins,
and loop exits. The analysis is the most complex and most conservative part of
the type checker, and its facts are fragile because bindings are mutable and
structs/arrays are shared handles.

## Direction

Replace automatic narrowing with explicit, syntax-forced unwrapping in the
style of Rust's `Option`: pattern binding and early return. No compile-time
flow analysis is kept, and no unchecked non-null assertion is introduced.

## Syntax additions

### 1. `if let` and `while let`

```cd
if let user = findUser(1) {
  print user.name;
} else {
  print "missing";
}

while let current = next {
  process(current);
  next = current.next;
}
```

Grammar:

```text
ifLetStmt    = "if", "let", identifier, "=", expression, block,
               [ "else", block ] ;
whileLetStmt = "while", "let", identifier, "=", expression, block ;
```

The binding is a fresh, scope-local binding whose type is the unwrapped
non-nil type (or unknown when the scrutinee type is unknown). The scrutinee
must be `optional<T>` or unknown; binding a non-optional value is a type
error. `if let` desugars to a `match` with `nil` and binding arms; `while let`
desugars to a `while (true)` whose body begins with that match and breaks on
the nil arm. The desugaring is semantic, not textual: the checker records the
binding through the existing pattern-binding path, and the IR lowers through
the existing match machinery.

### 2. `?` early-return unwrap

```cd
fun process(id: number): optional<string> {
  let user = findUser(id)?;
  return user.name;
}
```

`x?` requires `x : optional<T>`. It is allowed only inside a named function,
method, or anonymous function whose return type is `optional<U>` with `T`
assignable to `U` (or unknown `U`). Using `?` in a non-optional-returning
function, at top level, or on a non-optional value is a type error. Semantics:
evaluate `x`; if nil, return nil from the enclosing function; otherwise the
expression yields the non-nil value. The result type is `T`.

### 3. `??` nil-coalescing

```cd
let y = box.value ?? 0;
```

`left ?? right` requires `left : optional<T>` and `right : T` (or unknown).
The result type is `T`; if `left` is nil, `right` is evaluated and yielded.
Right-associative: `a ?? b ?? c` groups as `a ?? (b ?? c)`.

### 4. Optional type sugar (optional)

`number?` is sugar for `optional<number>` in type position. `optional<T>`
remains the canonical text form. This sugar is included only if the `?`
postfix expression operator does not create ambiguity in the parser; it is
otherwise deferred.

## Deleted machinery

- `FlowFacts` and `TypeCheckerFlow.cpp`, including `flowFacts_` state and its
  save/restore in module checking.
- Nil-check narrowing for `if`/`while`/C-style `for` conditions.
- Truthiness-based narrowing in `if (value)` then branches.
- Named-struct field, nested field-path, and array-index fact tracking and
  invalidation.
- Assignment, compound-assignment, index-write, direct/indirect/callback
  call, struct-method, and closure-boundary invalidation rules.
- Branch join, no-else continuation, loop-exit, and break/continue fact
  handling.
- The corresponding `nullable_narrowing_*` fixtures and the AGENTS.md
  semantics paragraphs.

## Kept semantics

- `optional<T>`, `nil`, nil comparisons, and assignability rules stay.
- `match` statement arms (including `nil` and binding patterns) stay and
  become the canonical nil-branching construct.
- `==`/`!=` on optional values remain ordinary boolean expressions with no
  type effect.
- Structs and arrays remain shared mutable handles; `if let`/`while let`
  bindings are fresh copies of the unwrapped handle, so their nil-ness fact
  cannot be invalidated by later aliasing. Deep mutation through aliases is
  unchanged and is not part of this design.

## Type-checking and lowering notes

- `if let`/`while let` reuse the existing match pattern-binding records, so
  binding ids, captures, and IR register bindings work unchanged.
- `??` and `?` are new expression nodes lowered to conditional branches in
  IR (nil test, copy, fallback/return). No new bytecode opcode is required;
  the `.cdbc 0.1` artifact surface is unchanged.
- `?` records the implicit `return nil` as an ordinary return so
  function-return checking (including implicit nil return) stays consistent.

## Library migration

- `if (x == nil) { return nil; } let v = x;` becomes `let v = x?;` in
  optional-returning functions.
- `if (x == nil) { return default; }` becomes `x ?? default`.
- `while (current != nil) { use current; current = current.next; }` becomes
  `while let current = ...` with a mutable driver binding, or an explicit
  match with a nil-break arm.
- Functions that only test nil without unwrapping keep plain `== nil`
  comparisons.

## Fixture plan

- Positive: `if let` with optional struct/primitive values, `while let` over a
  linked chain, `??` defaults, `?` in optional-returning functions and
  methods, nested `if let` inside `else` blocks.
- Negative: `if let` on non-optional, `??` with non-optional left or
  mismatched right, `?` at top level, `?` in non-optional-returning
  functions, `?` on non-optional values.
- Migrate or delete the 21 `nullable_narrowing_*`/flow fixtures; audit
  fixtures that use optional values without narrowing (they should be
  unaffected).
- Library data-structure tests must pass unchanged after the migration.

## Verification gate

Same canonical suite: ctest, golden, bytecode artifact/module/cache,
LSP/debugger, Rust VM goldens, Cargo tests, verification inventory, boundary,
malformed, and `git diff --check`. `.cdbc` text output must not change for
programs that do not use the new constructs.

## Non-goals

- No `!` non-null assertion (unchecked runtime failure) in the initial slice.
- No ownership/borrowing rules; aliasing stays as documented today.
- No `optional<T>` method surface (`value`, `valueOr`, `map`, `and_then`) in
  the initial slice; `??` and match cover the current needs.
- No full pattern bindings in `if let` beyond a single identifier; richer
  forms continue to use `match`.
