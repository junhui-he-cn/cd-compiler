# VM-4C-001：structured diagnostic kind boundary

Status: first structured diagnostic slice implemented on 2026-07-30 after the
library, linker-report, debugger, and profile boundaries. This slice makes the
existing typed error domains easier to consume without changing their display
text or the `.cdbc 0.1` format.

## Decision

The Rust library keeps separate typed error domains because artifact loading,
module linking, and execution have different structured context:

| Domain | Stable fields |
| --- | --- |
| `ArtifactError` | `kind`, one-based `line`, `message` |
| `LinkError` | `kind`, optional `module_identity`, optional `dependency_index`, `message` |
| `RuntimeError` | `kind`, `message`, optional `DebugLocation`, ordered `StackFrame`s, `DebugSource`s |

`ArtifactErrorKind::as_str()`, `LinkErrorKind::as_str()`,
`RuntimeErrorKind::as_str()`, and `ResourceKind::as_str()` provide stable
machine-readable labels. The labels use lowercase snake case for enum variants;
resource labels retain the existing human-readable phrases because they are
also part of resource-limit diagnostics:

```text
ArtifactErrorKind::UnsupportedVersion -> "unsupported_version"
LinkErrorKind::MissingDependency       -> "missing_dependency"
RuntimeErrorKind::Resource(_)          -> "resource"
ResourceKind::OutputBytes              -> "output bytes"
```

Runtime locations and frame locations continue to reference the artifact's
source-table index and source-local line/column/range. The `DebugSource.path`
and `DebugSource.module` values are preserved in the structured value; host
canonicalization is a CLI/display concern only. `Display` keeps the existing
human-readable parser, linker, and runtime diagnostics so current CLI stderr
and golden/error consumers do not change.

## Compatibility and non-goals

The new methods are additive library APIs. They do not add a JSON dependency,
serialize a new artifact section, or merge the three error domains into a
large enum with optional fields. The CLI continues to render typed failures
through its existing text paths and exit codes; hosts that need machine data
should use the library boundary.

This slice does not add source-map reconstruction, expression evaluation,
diagnostic localization, a versioned JSON schema, or path rewriting. Those
would require a separate host-facing protocol decision.

## Migration and deletion condition

No old error path is deleted. Existing `ParseError` and string-returning
compatibility functions remain available; checked artifact/link APIs and the
typed runtime result are the migration path for embedders. A future unified
diagnostic enum may only replace these domains after all three contexts have a
versioned schema and the CLI/test consumers migrate together.

## Evidence

`vm-rs/tests/library_api.rs` validates all stable kind labels and a runtime
failure carrying source identity, location, ordered call frames, and the
existing display text. Existing artifact/link malformed and CLI gates continue
to cover rendering and rejection behavior:

```sh
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
```
