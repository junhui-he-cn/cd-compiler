# Library TODO

This file records library work that remains after the language capability and
ordering-operator slices. It is not a promise that the current runtime
representation will be the permanent API.

The language-side decisions are tracked by
[Issue #11](https://github.com/junhui-he-cn/my-compiler/issues/11) for hashing
and [Issue #10](https://github.com/junhui-he-cn/my-compiler/issues/10) for
operator dispatch. Both issues now have implemented baseline slices.
Issue #15 defines the stable key contract for the generic hash containers.

## Language support already available

- [x] Compile-time `Eq`, `Ord`, and `Hash` capability bounds.
- [x] Deterministic `hash(value)` shared by the C++ compiler and Rust VM.
- [x] Builtin string ordering and named-struct ordering operators.
- [x] Public-interface and module-cache propagation for the implemented shapes.
- [x] Stable mutable recursive node handles, alias-visible field/link mutation,
      unlink-with-live-handle behavior, and cycle-safe formatting.

## Remaining library decisions

- [x] Define generic map keys through `Eq + Hash` with identity-stable mutable
      reference semantics and explicit aliasing behavior (Issue #15).
- [x] Implement array-backed `HashSet<T: Eq + Hash>` and
      `HashMap<K: Eq + Hash, V>` with explicit empty/missing behavior.
- [ ] Revisit hash-dependent algorithms such as Rabin–Karp when they provide a
      meaningful benefit over the current deterministic scans.
- [ ] Define whether user-defined struct operators can satisfy generic
      `T: Eq`/`T: Ord` through an explicit static witness or specialization rule.
- [ ] Decide whether to migrate LRU/LFU internals to node handles; preserve the
      current array-backed APIs until key ownership, eviction, and strong-cycle
      behavior are specified for that separate slice.

The built-in map remains primitive-keyed; generic hash containers use their own
array-backed bucket tables and the stable key contract above. Other library
structures continue to use explicit equality or `less` callbacks where that is
the more appropriate API.
