# M3A-INTERFACE-003: canonical in-memory interface ordering

Status: implemented against reference commit `40de24d`.

## Decision

`TypeChecker::moduleInterfaces()` returns a canonical in-memory order:

- module interfaces are ordered by snapshot-local `moduleId`;
- exported values, structs, and enums are ordered by public name; and
- methods within each exported struct are ordered by method name.

Field order, enum variant order, generic parameter order, and dependency-edge
order remain their source/graph declaration order because those sequences carry
semantic or diagnostic meaning. The ordering pass is applied while each
interface is constructed and does not alter `ModuleSymbols`, type checking,
lowering, or the stable text emitter (which already sorts its presentation
copies). Finalization only orders the module vector and does not rebuild
completed module interfaces.

## Rationale and migration boundary

The existing export tables use unordered maps, so an interface object built
directly from them could expose a different vector order across equivalent
processes even though `--module-interface` text was stable. Canonicalizing the
object itself makes graph/interface shadow comparisons reproducible before
importer consumers are migrated.

The focused `ir_source_location` fixture checks module ID order and an exported
value pair whose source/export order differs from canonical name order. The M0D
inventory remains revision `m0d-2026-07-22-r1` with 1,745 cases; focused CTest,
inventory validation, and the full parity/canonical commands form the release
gate.

No old production path is deleted. The emitter's presentation sorting remains
temporarily duplicated until every interface consumer uses canonical vectors
and a later decision proves that the compatibility sort can be removed without
changing output.

## Explicitly deferred

This slice does not define serialized interface ordering, interface hashes,
cache keys, imported symbol materialization, package resolution, or separate
compilation.
