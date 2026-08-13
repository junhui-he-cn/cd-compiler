# M9-LANG-CAP-001: retain the Eq/Ord/Hash capability constraints

Status: decided on 2026-08-13.

## Question

Should generic capability constraints (`Eq`, `Ord`, `Hash`, including
`A + B` constraint sets) be removed or reduced?

## Decision

Keep the capability constraint system as the compile-time contract for
equality, ordering, and hashing. Concrete types satisfy capabilities through
the built-in rules: `number` and `string` satisfy `Ord`; `Eq` covers known
non-unknown kinds including recursive `optional<T>` unwrapping; `Hash` covers
the hashable primitive kinds plus `optional<T>` unwrapping; type parameters
inherit their declared constraints. Constraints are erased at runtime, so no
artifact or `.cdbc` change is introduced.

The struct `Ord` witness recorded by M8-LANG-CAP-ORD-001 is revoked: after
the struct ordering operators were removed, custom structs no longer satisfy
`Ord`, and `Ord` is satisfied only by `number` and `string`.

## Evidence

- Equality and ordering binary operators validate operand type parameters
  against `Eq` and `Ord` respectively.
- The native `hash(value)` builtin requires its argument to satisfy `Hash`.
- The standard library constrains generics with these capabilities: `Eq` on
  `linearSearch`, `countValue`, `listIsPalindrome`, `uniqueValues`,
  `intersectionValues`, `unionValues`, and `newSet`; `Eq + Hash` on
  `HashSet`, `HashMap`, `newHashSet`, and `newHashMap`.
- Fixtures cover positive and negative cases: `generic_constraints`,
  `generic_capabilities`, and the `generic_capability_*` type errors.
- The canonical suite passes after the struct-witness removal: verification
  1868/1868, golden 791/791, CTest 47/47, module artifact/cache, boundary,
  and malformed gates.

## Boundary

No new syntax, no runtime witness bindings, and no `.cdbc` change. This
decision does not reopen `==`/`!=` overloading, arithmetic or unary overloads,
traits/interface dispatch, or monomorphization; those remain deferred.
