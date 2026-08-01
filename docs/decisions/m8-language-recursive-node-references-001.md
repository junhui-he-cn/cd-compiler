# M8-LANG-REF-001: stable node references and recursive mutable structs

Status: proposed for issue #16.

## Question

How can the language express mutable linked nodes and node handles without
introducing a second reference representation that disagrees with existing
struct aliases, artifacts, or the Rust VM?

## Decision

Permit recursive nominal struct field types and use the existing named struct
value as the stable handle. `optional<Node<T>>` represents an absent link.
Struct assignment, parameter passing, closure capture, and field storage copy
the identity-bearing handle. Field mutation is visible through all aliases.

The C++ runtime and Rust VM retain strong shared ownership. A node unlinked by
assigning `nil` remains valid if another handle still references it. Strong
cycles are accepted and are retained until VM teardown; weak references and
cycle collection are outside this decision.

Struct equality and hashing remain identity-based. C++ and Rust formatting keep
the existing acyclic output and emit `<cycle>` on an active-path identity
repeat. No new source syntax, IR opcode, bytecode opcode, or `cdbc` version is
introduced.

## Non-goals

This decision does not define `sizeof`, alignment, packed layout, borrow
checking, finalizers, weak references, automatic node deletion, or node-based
cache key ownership.

## Gate

The slice is admissible only when the recursive type fixtures, C++ value tests,
Rust VM value tests, imported interface/.cdi round trips, bytecode artifact
execution, and full repository verification pass with `git diff --check`.
