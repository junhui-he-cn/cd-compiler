# String For-In Iteration

Status: accepted and implemented on 2026-08-24.

## Decision

The existing `for-in` statement accepts statically known strings. Iteration
uses Unicode scalar-value order, and each loop binding has type `string`:

```cd
for character in "A🙂中" {
  print(character);
}
```

The Rust VM snapshots the scalar sequence when `iter_init` runs. Strings are
immutable, so iteration cannot observe source mutation. Combining marks remain
separate scalar values, matching the existing `len`, `substr`, and `charAt`
contract; grapheme segmentation and Unicode normalization remain out of scope.

## Compatibility

This slice reuses `iter_init`, `iter_has`, and `iter_next`. It adds no source
syntax, AST node, IR operation, bytecode opcode, artifact version, or public
iterator value. Dynamic values are checked by the existing VM error path.
Generic `Iterable<T>` / `Iterator<T>` declarations, user-defined iterators,
capability witnesses, and dynamic dispatch remain deferred.

## Verification

`tests/golden/for_in_strings` covers ASCII, supplementary-plane, and CJK
scalar values, including the statically inferred `string` loop binding. The
static and dynamic non-iterable fixtures cover the updated diagnostics. The
existing bytecode and Rust VM iterator paths continue to exercise the shared
protocol for arrays, maps, and ranges.
