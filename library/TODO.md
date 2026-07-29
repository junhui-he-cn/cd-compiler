# Library TODO

This file records library work that is intentionally waiting on a language
decision. It is not a promise that the current runtime representation will be
the permanent API.

The language-side request is tracked in
[Issue #11](https://github.com/junhui-he-cn/my-compiler/issues/11). The related
general operator-overloading request is [Issue #10](https://github.com/junhui-he-cn/my-compiler/issues/10).

## Waiting for language support: hash-based generic collections

- [ ] Add `Eq<T>` as a usable generic capability for equality-based APIs.
- [ ] Add `Hash<T>` as a usable generic capability for hash-based APIs.
- [ ] Add generic capability bounds, for example `HashSet<T: Eq + Hash>` and
      `HashMap<K: Eq + Hash, V>`.
- [ ] Define whether `==`/`!=` dispatch through the equality capability, while
      keeping the general operator-overloading design tracked by Issue #10.
- [ ] Provide a language-level `hash(value)` operation or an equivalent
      protocol entry point.
- [ ] Specify one deterministic hash algorithm shared by the C++ compiler and
      Rust VM, including the behavior for strings, numbers, booleans, enums,
      and user-defined structs.
- [ ] Define module-interface and cross-module rules for `Eq`/`Hash` bounds.
- [ ] Define how mutable values used as keys remain valid, or make key values
      immutable for the lifetime of their membership.

Once these conditions are available, implement and test:

- [ ] `HashSet<T>` and `HashMap<K, V>` with explicit empty/missing behavior.
- [ ] Bloom filter and other hash-based optional topics.
- [ ] Hash-dependent string algorithms such as Rabin–Karp where they improve
      the library meaningfully.
- [ ] A cache implementation only after the key mutation and ownership rules
      are stable.

Until then, the library uses array-backed equality/comparator implementations
and integer-vertex graph APIs that do not require a user-defined hash
contract.
