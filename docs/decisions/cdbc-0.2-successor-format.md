# `cdbc 0.2` Successor Format Decision

Status: implemented on 2026-08-15 via merge `83d7c8e6` (feature branch
`feat/bytecode-0.2-phase0`, Phases 0-14).

## Decision

The emitted `.cdbc` artifact version moves from `cdbc 0.1` to `cdbc 0.2`. The
C++ backend now emits `cdbc 0.2` bodies composed of `block bN:` sections with
explicit terminators, numeric local/upvalue/global slots, numeric function and
direct-call targets, specialized collection access/length and arithmetic or
ordered-comparison opcodes, numeric struct/enum type and layout tables, native
imports lowered to index-based `call_native`, module init functions instead of
module stream splicing, an iterator protocol for `for-in`, and `max_digits10`
`f64` constants. The Rust VM parser, formatter, and verifier accept and
validate `cdbc 0.2` only; `cdbc 0.1` headers are rejected as unsupported
versions before any body parsing. The C++ compiler emits only `cdbc 0.2`.

This supersedes the "selected no successor version" conclusion of
`m05b-cdbc-contract.md`: the breaking opcode/value-layout changes could not be
expressed as compatible `cdbc 0.1` extensions, which is exactly the successor
condition that record named.

## Compatibility and read path

- The C++ compiler emits only `cdbc 0.2`.
- The Rust VM accepts only the `cdbc 0.2` header; legacy name-driven
  `load_var`/`store_var`/`jump*`/`print` opcodes and `cdbc 0.1` inputs are
  rejected. Dynamic fallbacks (`field`, `assign_field`, generic `index`/
  `assign_index`/`len`, and generic arithmetic/comparison) remain part of the
  0.2 instruction set for untyped or generic bodies.
- Debug source/location/range sections and the separate `cdbc-cache 0.2`
  manifest are unchanged by the format upgrade.
- The X1 compatibility matrix is pinned to the Phase 7 native-import delivery
  in `x1-compiler-vm-compatibility-001.json`.

## Authoritative documents

- Format spec: `docs/bytecode-text-format.md`.
- Chinese instruction reference: `docs/bytecode-instructions-zh.md`.
- Design plan and baseline: `docs/cd-compiler-bytecode-0.2-execution-plan.md`
  and `docs/cd-compiler-bytecode-0.2-baseline.md`.
- Version policy: `docs/versioning.md` (`VERSION` is now `0.2.0`).

## Migration and deletion rules

The former `cdbc 0.1` read path and its fixtures were removed on 2026-08-15:
the VM now rejects legacy headers as unsupported versions before execution.
Malformed-version handling remains strict for unknown `cdbc` versions.

## Evidence

The upgrade carries its own golden, artifact, module, malformed, compatibility,
and Rust VM coverage, including the `vm-rs/tests/bytecode_0_2_baseline.rs`
0.2 boundary and legacy-rejection tests, together with the refreshed
`docs/bytecode-text-format.md` and `docs/bytecode-instructions-zh.md`
specifications.
