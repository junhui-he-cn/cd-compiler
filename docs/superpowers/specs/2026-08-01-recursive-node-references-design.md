# Recursive node references design

Issue: #16
Status: proposed implementation slice

## Decision

Use the existing nominal struct value as the language's stable node handle.
Named structs are already identity-bearing reference values: the C++ runtime
stores their fields behind `std::shared_ptr`, and the Rust VM stores them
behind `Rc<RefCell<...>>`. A variable, argument, closure capture, or struct
field therefore copies a handle and not the node payload.

Allow a struct field to refer to the struct being checked or to another struct
in the same recursive declaration group. The recursive type description stays
finite because `TypeInfo` records a nominal name and type arguments rather than
embedding the referenced struct's fields. Existing `optional<T>` syntax
expresses an empty link; no `ref<T>` syntax or new bytecode operation is
required.

## Runtime contract

- Field writes are visible through every alias to the same struct identity.
- Assigning `nil` to a link removes that edge. It does not invalidate an
  existing handle to the former target.
- Handles are strong. A node remains alive while directly held or reachable
  through strong fields. Cycles are valid and remain retained until VM teardown
  because this slice does not add weak references or cycle collection.
- Struct equality and hashing use the existing object identity. They never
  recursively traverse fields.
- Acyclic text formatting is unchanged. If an array, map, or struct identity
  is encountered again on the active formatting path, formatting emits the
  deterministic marker `<cycle>` and stops descending that edge.

## Boundaries

This slice does not add borrow checking, finalizers, weak handles, a memory
layout or `sizeof` contract, automatic deletion, iterator invalidation rules,
or node-based cache APIs. The library can build linked structures on this
contract; cache migration remains a separate library slice.

## Verification

The proof corpus covers self-recursive, generic, array/function-shaped, and
mutually recursive declarations; link deletion with a retained alias; alias
mutation; identity equality; C++ and Rust cycle-safe formatting; exported
recursive shapes through `--module-interface` and `.cdi`; and Rust VM execution
of the unchanged struct field bytecode.
