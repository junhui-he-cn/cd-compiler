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
- [x] Complete named-struct ordering operators provide a static `T: Ord`
      witness (and therefore `T: Eq`); partial sets remain direct-only (Issue #17).
- [x] Migrate LRU internals to private singly-linked node handles while
      preserving the public API, `K: Eq` contract, snapshot copies, and explicit
      detach-on-eviction behavior.
- [x] Migrate LFU internals to private singly-linked node handles while
      preserving the public API, storage-order snapshots, frequency tie-breaks,
      and explicit detach-on-eviction behavior.
- [x] Evaluate the node-backed LFU against the previous array backend with a
      repeatable scaled workload; the node version reduces profiled instruction
      count but increases allocation pressure and is slower in wall-clock runtime.
      Keep the current node-backed API and defer frequency buckets or an indexed
      backend until a larger real workload justifies that representation.

The built-in map remains primitive-keyed; generic hash containers use their own
array-backed bucket tables and the stable key contract above. LRU and LFU use
equality lookup plus private one-way nodes and therefore do not add a `Hash`
requirement; the high-performance LFU backend remains deferred. Other library
structures continue to use their existing array backends or explicit equality/
`less` callbacks where that is the more appropriate API.
