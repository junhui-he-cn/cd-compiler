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
validate `cdbc 0.2` and keep a read-compatibility path for `cdbc 0.1`. The
C++ compiler emits only `cdbc 0.2`; `dump` round-trips a legacy artifact with
its `cdbc 0.1` header when legacy instructions remain.

This supersedes the "selected no successor version" conclusion of
`m05b-cdbc-contract.md`: the breaking opcode/value-layout changes could not be
expressed as compatible `cdbc 0.1` extensions, which is exactly the successor
condition that record named.

## Compatibility and read path

- The C++ compiler emits only `cdbc 0.2`.
- The Rust VM reads both `cdbc 0.1` and `cdbc 0.2` headers; the 0.1 path keeps
  the legacy name-driven `load_var`/`store_var`/`jump*`/`print`/`index`/
  `assign_index`/`len` and generic arithmetic/comparison instructions.
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

No `cdbc 0.1` reader or fixture path is deleted. Legacy artifacts continue to
execute through the compatibility path, and malformed-version handling remains
strict before VM execution. Removing the 0.1 read path later requires a
separate deprecation decision with an updated compatibility matrix and support
lifecycle.

## Evidence

The upgrade carries its own golden, artifact, module, malformed, compatibility,
and Rust VM coverage, including the `vm-rs/tests/bytecode_0_2_baseline.rs`
0.1/0.2 behavior baseline, together with the refreshed
`docs/bytecode-text-format.md` and `docs/bytecode-instructions-zh.md`
specifications.
