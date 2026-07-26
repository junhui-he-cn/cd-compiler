# M3A-INTERFACE-005: per-module in-memory interface production

Status: implemented against reference commit `e02b41e`.

## Decision

`TypeChecker` produces a module's in-memory `ModuleInterface` immediately after
`checkModule` finishes checking that module body and its imports:

- `buildModuleInterface` snapshots the completed module's graph identity,
  dependency edges, and public `ModuleSymbols` records;
- values, structs, enums, and methods are canonicalized while that module
  interface is created;
- direct imports, namespace imports, and re-exports consume the already-built
  target interface; and
- final `buildModuleInterfaces` materialization only fills a missing module
  record defensively, then orders the module vector by module ID.

This removes the previous full-program interface rebuild from every import and
re-export operation. It does not change the producer source: dependency bodies
are still checked in the current process and `ModuleSymbols` remains the
authoritative public-export table for that check.

## Compatibility and migration

The slice is behavior-preserving. The graph identity, dependency metadata,
canonical interface vectors, import visibility, re-export diagnostics,
resolved lowering names, `--module-interface` text, IR, bytecode, `.cdbc`, and
Rust VM behavior retain their existing contracts. The three-module
`lib -> api -> entry` source-metadata test explicitly exercises the intermediate
re-export interface before the entry import is checked.

The M0D inventory remains revision `m0d-2026-07-22-r1` with 1,745 cases.
Focused CTest, inventory validation, and the full parity/canonical commands
form the release gate. No serialized interface, persistent cache key,
cross-build identity, independent module result, or per-module `.cdbc` product
is introduced.

## Old-path deletion condition

The final missing-interface fallback may be removed after the checked-module
invariant is asserted for every graph module in the canonical inventory and
two verification runs show that every importer observes an interface produced
at its dependency completion boundary. Dependency-body checking and lowering
remain governed by M3B and are not deleted here.

## Explicitly deferred

This slice does not define interface hashes or invalidation, load serialized
interfaces, change cycle policy, remove dependency-body checking, or choose
the M3B per-module artifact/linking model.
