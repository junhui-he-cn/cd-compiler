# M3B-ARTIFACT-001: independently validated module `.cdbc` products

Status: implemented as the first M3B/M4A artifact slice against the current
`cdbc 0.1` contract.

## Decision

Independent compilation produces one `.cdbc` product per graph module. The
products remain in the existing `cdbc 0.1` family and use a strict
`artifact: module` envelope rather than changing the linked-program format.

Each product contains:

- graph canonical-path `identity`, display `path`, and `canonical_path`;
- `entry` plus `entry_order` for deterministic entry selection;
- source-ordered import/re-export dependencies, target identities, requested
  paths, and local `main` instruction insertion offsets; and
- ordinary constants, names, main, function, and optional debug sections for
  only that module's lowered body.

The C++ compiler exposes this path as:

```text
compiler_design --emit-module-bytecode <directory> <entry.cd> [...]
```

It creates `module-<snapshot-module-id>.cdbc` files in graph-node order. The
numeric filename is a session-local transport name; the serialized identity is
the canonical-path product key. `IRCompiler::compileModule` lowers one module
body and records import/re-export markers instead of recursively lowering
dependency bodies. `--emit-bytecode` continues to produce the existing linked
program artifact.

The Rust parser validates and canonicalizes both artifact kinds. The VM dump
command accepts module products for validation; the VM run command rejects an
unlinked module product. The VM `link <module-directory> <output.cdbc>` command
selects entry products by `entry_order`, walks dependency markers in source
order, expands each identity once, and rebases local bytecode references while
preserving insertion offsets.

## Compatibility and migration

The linked `cdbc 0.1` header and section order remain byte-for-byte compatible.
Old linked artifacts have no artifact declaration and parse as final programs.
Module products add only the recognized module envelope before the existing
core sections. The Rust parser rejects malformed module fields, dependency
references, empty identities/paths, out-of-range or decreasing insertion
offsets, unknown sections, and attempts to execute a module product.

This slice intentionally keeps same-process TypeChecker interface production
and the existing linked lowering path. It does not serialize interfaces, add
cache keys or invalidation, or claim that snapshot-local numeric module IDs are
persistent identities. The Rust linker is part of the module-product proof and
merges validated products into the existing final linked artifact.

## Verification

- `bytecode_module_emitter_tests` proves the C++ envelope and linked-output
  compatibility.
- `tests/bytecode_module_artifact_tests.py` emits the
  `module_import_order` graph, validates both products through Rust `dump`,
  checks that the entry product contains a marker rather than dependency body
  instructions, and checks that unlinked products are rejected by `run`.
- Rust format unit tests cover empty module dependency lists, entry order,
  dependency kinds, and canonical round trips.

The follow-up `M3B-CACHE-001` slice owns incremental cache keys, invalidation,
and rebuild measurement for the product set.
