# M3A-INTERFACE-004: in-memory interface importer consumption

Status: implemented against reference commit `a7b4789`.

## Decision

After an imported or re-exported module has been checked by the current
whole-program producer path, `TypeChecker` builds its `ModuleInterface` and
uses that interface's public data for the importer operation:

- direct and namespace imports read exported values, structs, enums, and
  methods from the interface;
- re-exports select values, structs, enums, and struct methods from the target
  interface; and
- interface values retain the snapshot-local resolved name needed by the
  current same-program lowering path, while methods retain receiver and
  resolved-name metadata.

The existing `ModuleSymbols` tables remain the producer-side export state and
the current module's local recording state. `findModule` and `checkModule`
still locate/check dependency bodies before an interface is produced; this is
the deliberate M3A migration boundary. Once the interface exists, importer
public shape and method metadata do not reread the dependency's declaration
body for that import operation.

## Compatibility and migration

This is an in-memory same-process cutover. It does not claim separate
compilation: resolved names and type declarations remain snapshot-local, and
dependency bodies are still checked to produce the interface. The existing
diagnostic tokens, namespace qualification, generic substitution, re-export
visibility, IR lowering, `.cdbc` output, and Rust VM behavior remain
authoritative and are covered by the existing corpus.

The focused module-interface fixture covers direct import and re-export
consumption. The full inventory additionally exercises namespace imports,
generic functions/enums/structs, imported methods, and re-exported methods. The
M0D inventory remains revision `m0d-2026-07-22-r1` with 1,745 cases; CTest,
legacy parity, inventory, boundary, malformed, and canonical gates are the
release criteria.

No old path is deleted in this slice. Dependency-body checking remains until
M3B's independent module result and cache decisions exist. The `ModuleSymbols`
producer/recording path may be reduced only after interface-derived public
metadata and the current lowering identities have an explicit replacement.

## Explicitly deferred

This slice does not load serialized interfaces, define interface versioning or
cache keys, assign cross-build symbol identities, change cycle policy, or emit
per-module `.cdbc` artifacts.
