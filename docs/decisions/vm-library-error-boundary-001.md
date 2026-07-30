# VM-3A-002：typed library error and version boundary

Status: implemented on 2026-07-30 as an additive follow-up to
`VM-3A-001`. The Rust VM remains a private binary package with an embeddable
library target; this slice makes the first library failure and version fields
usable without changing the existing CLI or compatibility functions.

## Decision

The top-level library facade publishes `LIBRARY_API_VERSION = "0.1"` and the
artifact module publishes the canonical `ARTIFACT_FORMAT_FAMILY = "cdbc"`,
`ARTIFACT_FORMAT_VERSION = "0.1"`, and `ARTIFACT_HEADER = "cdbc 0.1"`.
These constants describe the current API/artifact boundary; they do not make a
crates.io or SemVer compatibility promise while `publish = false` remains in
force.

Typed artifact loading and verification use the additive checked functions:

- `parse_artifact_checked` returns `ArtifactError` with `Parse`,
  `UnsupportedVersion`, or `Verification` kind, a one-based artifact line, and
  the existing human-readable message;
- `verify_artifact_checked`, `verify_program_checked`, and
  `verify_module_artifact_checked` use the same verification error shape; and
- `ArtifactError` implements `Display` without changing the legacy
  `ParseError` text.

Typed module linking uses `LinkError` and `LinkErrorKind` through
`link_modules_checked` and `link_modules_with_report_checked`. Link errors
carry an optional module identity and dependency index when the failing input
has that context. The checked linker classifies invalid module products,
duplicate/empty/entry errors, missing or invalid dependencies, cycles,
instruction/size overflow, and invalid linked programs. `Display` preserves
the existing deterministic linker message.

The existing `parse_artifact`, `verify_*`, `link_modules`, and
`link_modules_with_report` functions remain available with their original
return types. The legacy linker functions are compatibility adapters over the
typed implementation and convert only at the outer boundary. The CLI keeps
using these compatibility functions, so command arguments, exit codes,
stdout, stderr, artifact bytes, and `cdbc 0.1` acceptance are unchanged.

## Boundary and non-goals

This slice does not add a structured CLI protocol, persistent VM sessions,
host-retained runtime values, native/plugin registration, `Send`/`Sync`, a
binary artifact, a successor artifact version, or a new runtime error model.
`RuntimeError` already exposes a typed kind, source location, stack, and source
table and remains unchanged here. Version negotiation and migration policy
for a future artifact successor remain VM-3C work and require benchmark or
compatibility evidence.

## Migration and deletion condition

New embedders should use the checked functions and inspect `kind`,
`module_identity`, and `dependency_index` rather than parse diagnostic text.
Existing callers may keep the legacy functions while migrating. The adapters
and `ParseError` boundary may only be removed after an external embedding
consumer has migrated, CLI compatibility is covered by the artifact/module
goldens, and a separately reviewed API/version migration policy exists.

## Evidence

`vm-rs/tests/library_api.rs` proves the version constants, unsupported-version
classification, verification classification, typed missing-dependency context,
legacy linker display compatibility, in-memory execution, trace, module link,
and VM-instance isolation.

```sh
cargo test --manifest-path vm-rs/Cargo.toml
```
