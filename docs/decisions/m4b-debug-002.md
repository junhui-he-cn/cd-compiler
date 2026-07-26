# M4B-DEBUG-002: persistent source-to-module debug identity

Status: implemented against the existing cdbc 0.1 debug metadata contract.

## Decision

Keep debug_sources and debug_locations in cdbc 0.1 and add one optional
module field to each debug source entry:

    s0 module="/canonical/lib.cd" path="lib.cd" text="..."

The field is emitted when an import-aware Program source maps to a
ModuleGraph node. The mapping uses SourceFile.id only inside the compiler
snapshot and serializes the graph node's canonicalPath, never the numeric
snapshot ID. Direct single-file and ordered multi-file inputs have no module
graph identity and retain the old source-entry form.

The C++ source metadata, IR/bytecode source tables, C++ emitter, Rust
DebugSource model, parser, formatter, and module linker share this optional
field. The Rust parser accepts both old metadata-free/module-free entries and
the new module-aware form, rejects an explicitly empty identity, and preserves
the identity while rebasing artifact-local source indexes during linking.
Runtime diagnostics continue to use path as their display source; module is
available to linkers and future debug consumers.

## Compatibility and migration

This is a compatible additive extension within cdbc 0.1. Existing linked
artifacts, metadata-free artifacts, and old debug source lines remain readable
and canonicalize byte-for-byte. Import-aware compiler output gains stable
module identity fields, while debug locations and runtime call-stack ordering
remain unchanged. No successor artifact version, persistent compiler ID, or
new runtime diagnostic format is introduced.

The C++ emitter and Rust formatter use the same field order:
module when present, then path, then text. Module-product linking copies the
field with each source table entry and rebases only source references.

## Quantitative gate

The focused conformance set proves:

- bytecode_module_emitter covers C++ emission and old output compatibility;
- Rust format unit tests cover old parsing, module-aware round trip, and empty
  identity rejection;
- bytecode_module_artifact_tests checks module products retain both canonical
  source identities and still dump/link/run;
- module_debug_metadata_tests checks linked identities, source paths, runtime
  locations, and deterministic inner-to-outer frames; and
- cdbc_contract_audit records the optional field in its capability inventory
  while the complete 58-artifact reference corpus still dumps byte-for-byte.

## Old-path deletion condition

Keep the optional module-free fallback until every supported compiler-emitted
debug consumer uses module identity or explicitly documents why a source has
no graph identity. Do not serialize snapshot-local SourceFileId values or
remove cdbc 0.1 compatibility as part of this slice.

## Explicitly deferred

Full source ranges, debugger protocol events, breakpoint tables, function
symbol identities, module identity hashes, and runtime diagnostics that display
module separately from path remain later M4B work.
