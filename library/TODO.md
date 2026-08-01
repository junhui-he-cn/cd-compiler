# Library TODO

This file records library work that remains after the language capability and
ordering-operator slices. It is not a promise that the current runtime
representation will be the permanent API.

The language-side decisions are tracked by
[Issue #11](https://github.com/junhui-he-cn/my-compiler/issues/11) for hashing
and [Issue #10](https://github.com/junhui-he-cn/my-compiler/issues/10) for
operator dispatch. Both issues now have implemented baseline slices.

## Language support already available

- [x] Compile-time `Eq`, `Ord`, and `Hash` capability bounds.
- [x] Deterministic `hash(value)` shared by the C++ compiler and Rust VM.
- [x] Builtin string ordering and named-struct ordering operators.
- [x] Public-interface and module-cache propagation for the implemented shapes.
- [x] Stable mutable recursive node handles, alias-visible field/link mutation,
      unlink-with-live-handle behavior, and cycle-safe formatting.

## Remaining library decisions

- [ ] Define how generic map keys use `Eq`/`Hash` without changing mutable-key
      ownership and aliasing rules.
- [ ] Implement `HashSet<T>` and `HashMap<K, V>` only after that key contract is
      stable, with explicit empty/missing behavior.
- [ ] Revisit hash-dependent algorithms such as Rabin–Karp when they provide a
      meaningful benefit over the current deterministic scans.
- [ ] Define whether user-defined struct operators can satisfy generic
      `T: Eq`/`T: Ord` through an explicit static witness or specialization rule.
- [ ] Decide whether to migrate LRU/LFU internals to node handles; preserve the
      current array-backed APIs until key ownership, eviction, and strong-cycle
      behavior are specified for that separate slice.

Until then, the library uses array-backed equality/comparator implementations,
explicit `less` callbacks, and integer-vertex graph APIs.
