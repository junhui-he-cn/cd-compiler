# M3A-INTERFACE-013: invalid-sidecar diagnostic and partial-reuse parity

Status: implemented against `M3A-INTERFACE-009` and the canonical cache
inventory introduced by `M3A-INTERFACE-012`.

## Decision

When an imported sidecar is rejected in default mode, the importer keeps the
ordinary source path authoritative for diagnostics. The cache matrix now
proves both parse and type failures from the changed imported source retain the
file-aware path and diagnostic kind. A valid lower dependency sidecar may still
be reused while an invalid importing sidecar is parsed from source; this is
checked as partial reuse, not as whole-program AST equality, because a valid
sidecar intentionally omits its private dependency body.

The two new canonical case IDs are:

- `module_cache.fallback.diagnostics`;
- `module_cache.fallback.partial_dependency`.

## Compatibility and migration

The language, default source fallback, `cdi 0.1`, and `cdbc 0.1` are unchanged.
The tests compare no-cache and malformed-cache diagnostics byte-for-byte for
the parse/type cases. The partial-reuse case checks successful importer
loading, omission of the valid lower dependency body, retention of the
invalid middle module's source body, and repeated deterministic output.

## Quantitative gate

The module-cache runner reports eight named cases in the M0A inventory: the
four graph fallback shapes, diagnostic parity, partial dependency reuse,
rebuild/reuse invalidation, and strict rejection. The focused runner and the
canonical runner must report all eight as passed with zero untracked results.

## Old-path deletion condition

Keep source fallback until the complete inventory covers successful visibility,
parse/type/import diagnostics, partial valid-dependency reuse, direct inputs,
and repeated builds with zero unexplained parity differences. This slice does
not remove fallback or dependency-body checking.

## Explicitly deferred

This slice does not change strict-cache diagnostics, add cache repair or remote
storage, validate module product contents in `FrontendSession`, or alter Rust
VM sidecar behavior.
