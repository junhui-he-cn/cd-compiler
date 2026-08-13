# M8-LANG-CAP-ORD-001: struct ordering capability witness

Status: implemented for issue #17; superseded by M9-LANG-CAP-001 (struct
ordering operators and their `Ord` witness were removed on 2026-08-13).

## Decision

A named struct satisfies generic `Ord` only when its defining module provides
all four valid ordering operators: `<`, `<=`, `>`, and `>=`. `Ord` implies
`Eq`. Existing identity equality remains the base `Eq` contract for structs,
including the `Eq + Hash` containers delivered by issue #15; a partial or
empty ordering set therefore remains valid for direct equality and direct
operators but not for `T: Ord`.

The existing nominal operator tables and public interface records are the
capability witness. Implementations remain owned by the defining module,
duplicate slots remain errors, and imports/re-exports consume the owner's
complete public set without creating foreign implementations.

## Runtime boundary

Generic ordering keeps the existing four comparison instructions. Complete
witness structs also publish private deterministic bindings named
`__capability_ord_<Struct>_<operator-name>`. The Rust VM resolves those
bindings only when erased generic comparison receives two same-named struct
values, then invokes the ordinary operator function and requires a boolean
result. No user-visible dictionary, new source syntax, or `cdbc` version is
introduced.

## Deferred

User-defined `==`/`!=`, arithmetic and unary overloads, dynamic dispatch,
trait objects, monomorphization, and automatic law checking remain deferred.

## Verification

The local and imported/re-exported witness fixtures cover all four comparisons,
identity equality, partial-witness rejection, module products, and Rust VM
execution. The current working tree passes canonical 1935/1935, golden
843/843, CTest 43/43, module artifact/cache, library 116/116, Rust VM 798/798,
Cargo 95/95, boundary 5/5, malformed 108/108, and `git diff --check`.
