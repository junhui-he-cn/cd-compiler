# M3A-INTERFACE-006: in-memory interface consistency validation

Status: implemented against reference commit `03e6083`.

## Decision

After final module-interface materialization, `TypeChecker` records a
snapshot-local mismatch count from `validateModuleInterfaces` and exposes it
through `moduleInterfaceMismatchCount()`.

The validator checks only deterministic structural facts needed by the current
M3A migration boundary:

- the interface module set and module-ID order match the `Program` module set;
- graph nodes, source identity, display/canonical paths, entry markers, and
  dependency edges match the `Program::moduleGraph` snapshot;
- values, structs, enums, struct methods, and struct operators are unique and
  in their canonical in-memory order; and
- the public-name sets match the producer-side `ModuleSymbols` export tables.

It is an observability gate, not a second semantic producer. It does not
compare or rewrite types, run module bodies again, or alter diagnostics and
outputs. A mismatch is retained for focused and canonical tests to assert as
zero rather than being silently converted into a user-facing error.

## Compatibility and migration

The validator runs after the existing checked-module producer and interface
consumer paths. It preserves direct single-file and ordered direct-multi-file
semantics, import search paths, re-exports, cycle rejection, file-aware
diagnostics, interface text, IR, bytecode, `.cdbc`, and Rust VM behavior.

The source-metadata import/re-export fixture asserts zero mismatches while
checking a three-module `lib -> api -> entry` chain. The M0D inventory remains
revision `m0d-2026-07-22-r1` with 1,745 cases. Focused CTest, inventory
validation, and full legacy/canonical/boundary/malformed gates are required.

## Old-path deletion condition

No production path is deleted here. The mismatch count becomes a required
zero gate for future interface consumers. A validator branch may be removed
only when its corresponding compatibility fallback is removed and the
replacement has an explicit M3A/M3B decision and proof.

## Explicitly deferred

This slice does not define type fingerprints, interface hashes, serialized
validation, persistent cache keys, cross-build identities, independent module
results, or per-module `.cdbc` artifacts.
