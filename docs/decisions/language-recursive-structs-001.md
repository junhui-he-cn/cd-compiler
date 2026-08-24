# Recursive Nominal Struct References

Status: implemented and reaffirmed on 2026-08-18.

## Decision

Named struct field types may contain finite recursive nominal handles. The
existing named struct value is the handle, so recursive fields can use forms
such as `optional<Node>`, arrays, function types, generic instantiations, and
mutually recursive named structs without a separate `ref<T>` syntax.

Struct assignment, parameter passing, closure capture, and field storage copy
the shared handle. Mutating a field is visible through aliases, and assigning
`nil` to a link does not invalidate another handle that still refers to the
node. Struct equality and hashing remain identity-based. Reachable strong
cycles remain alive while their roots are retained, and formatting a repeated
identity on the active path emits `<cycle>`.

## Compatibility

This is the existing nominal struct contract. It adds no source syntax, IR or
bytecode opcode, artifact version, or module-interface representation. The Rust
VM uses a non-moving tracing heap and reclaims unreachable tracked cycles at VM
safepoints without changing live object identity. The C++ `Value` layer uses
`shared_ptr` storage and provides cycle-safe formatting, but has no separate
cycle collector. Weak references, borrow checking, and finalizers remain out of
scope.

## Verification

`tests/golden/recursive_node_type_shapes` covers recursive nullable, array,
function, generic, and mutual shapes. `recursive_node_references` covers
alias-visible mutation, unlinking, identity equality, and cycle formatting;
`recursive_node_import` covers imported public shapes. The module artifact and
module-cache tests cover linked products and recursive public-shape invalidation.
`vm-rs/tests/cycle_lifetime.rs` covers rooted-cycle retention and collection
after the external root is dropped.
