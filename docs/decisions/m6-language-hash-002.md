# M6-LANG-HASH-002: stable generic hash keys

Status: implemented as the key-contract and public-library slice for issue
#15.

## Decision

Generic hash containers require both `Eq` and `Hash`. They use the language's
existing equality and deterministic hash operation; hash collisions are
resolved by equality. The built-in `map<K, V>` keeps its existing
`nil`/`number`/`bool`/`string` key restriction and is not silently widened by
this slice.

Reference values use identity-stable key semantics. Arrays, maps, functions,
and named structs compare and hash by their runtime identity. Mutating a
reference through an alias therefore does not invalidate a stored key, but the
mutation remains visible through every alias and through shallow snapshots.
The container does not copy, freeze, or deep-clone keys.

Value-like keys retain their existing semantics: `nil`, booleans, strings, and
numbers use value equality; `-0` and `+0` are equal and hash equally; ranges
use `(start, stop, step)` equality; enum variants compare structurally and
apply the same rule recursively to payloads. `optional<T>` keys use the same
rules for `nil` and the inner value. A valid key must be reflexive; the library
rejects a non-reflexive key such as NaN without changing the existing numeric
hash algorithm.

The key law is:

```text
a == b  =>  hash(a) == hash(b)
```

The converse is not required. A separately constructed array or struct with
the same visible contents is not equal to an existing identity key.

## Public library contract

`HashSet<T: Eq + Hash>` and `HashMap<K: Eq + Hash, V>` are array-backed bucket
tables implemented in `library/hash_collections.cd`; they do not depend on the
built-in map. Both preserve insertion order in `snapshot()` and return shallow
copies of their outer result arrays.

- `add`/`put` return `true` for a new entry and `false` for a duplicate or an
  invalid non-reflexive key; `put` updates an existing value without moving its
  key.
- `has` and `discard` return `false` for missing or invalid keys.
- `get` returns `nil` for a missing or invalid key; callers storing `nil`
  should use `has` to distinguish absence.
- `clear` removes all entries while retaining the current bucket capacity.
- Buckets resize above a 75% load factor and never expose bucket indexes.

The compiler reports a type error when a generic key parameter does not satisfy
`Eq + Hash`. Existing built-in map diagnostics continue to reject reference
keys with `map key must be nil, number, bool, or string`. User-defined
capability witnesses remain deferred to issue #17, and recursive mutable node
ownership remains deferred to issue #16.

## Verification boundary

The focused corpus covers primitive insertion/update/removal, resizing,
missing values, reference aliases, mutation after insertion, independently
constructed identity keys, the unconstrained generic diagnostic, and C++/Rust
hash stability for mutable reference values.
