# M4A-VALIDATION-001: `cdbc 0.1` compatibility matrix and pre-execution validation

Status: implemented against the existing `cdbc 0.1` contract.

## Decision

Keep the current versioned artifact family and make its acceptance boundary
explicit. The Rust format parser accepts exactly `cdbc 0.1`, validates the
decoded program before returning it, and leaves the existing C++ emitter and
canonical text unchanged. No successor version is selected.

| Header | Kind | `dump` | `run` | `link` |
| --- | --- | --- | --- | --- |
| `cdbc 0.1` | linked program | accept after validation | execute after validation | reject as non-module |
| `cdbc 0.1` | module product | accept after validation | reject before VM execution | accept only for a valid product set |
| other family/version | any | reject before decode | reject before VM execution | reject before loading |
| missing/unversioned | any | reject before decode | reject before VM execution | reject before loading |

Validation covers finite number constants, constant/name/function/register
references, jump targets, debug-location table shape, and the fixed native-call
capability set. Module identity, entry metadata, dependency targets, and
insertion offsets remain validated by the module envelope and linker checks.

## Compatibility and migration

This is a hardening slice within `0.1`. The C++ writer's core sections,
linked-program envelope, module-product envelope, Rust canonical dump, and
runtime ABI are unchanged for valid artifacts. M4B-DEBUG-002 later adds an
optional source-to-module field to import-aware debug source entries without
changing the cdbc version. Invalid references that previously could reach runtime now
fail during Rust decode, before `run` or module linking can execute or write a
program. Unknown family/version headers remain strict rejections.

A successor is deferred because validation does not require a new opcode,
value layout, target, transport, or negotiation field. M4A may select one only
for a concrete breaking compatibility requirement with a named producer,
consumer, migration path, and fixture set.

## Verification

- `vm-rs/src/format.rs` unit tests reject each cross-reference and unsupported
  native capability before execution.
- The linked artifact corpus remains byte-for-byte stable through
  `tests/bytecode_artifact_tests.py`.
- `tests/bytecode_module_artifact_tests.py` rejects invalid module offsets and
  missing entry order while preserving link/run parity for valid products.
- The malformed corpus adds invalid register, constant, jump, name, number,
  function, and native references; all are run twice and rejected by Rust
  `dump`.
- `tests/cdbc_contract_audit.py` records the reference dumps and compatibility
  rejection probes in a machine-readable report.

## Deletion condition

Do not remove the `cdbc 0.1` reader or emitter. A future older-path deletion
requires a separately versioned successor matrix, migration/rejection policy,
deprecation period, affected fixtures, and a passing canonical verification
run.

## Explicitly deferred

Successor-version negotiation, target/runtime identity, integrity metadata,
embedded framing, and changes to canonical text emission remain deferred.
