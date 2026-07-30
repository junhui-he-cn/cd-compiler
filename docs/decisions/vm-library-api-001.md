# VM-3A-001：Rust VM library and CLI boundary

Status: first library boundary implemented on 2026-07-30. The package keeps
the `compiler-design-vm` binary and now also emits a library target from the
same source modules.

## Decision

`vm-rs/src/lib.rs` is the embeddable boundary. It publishes the typed modules
for bytecode, artifact format, linking, runtime values, and VM execution, plus
these top-level names:

- `parse_artifact`, `parse_program`, `verify_artifact`,
  `verify_program`, `verify_module_artifact`, `format_artifact`, and
  `format_program`;
- `link_modules` and the `Artifact`/`ModuleArtifact`/dependency types;
- `Program`, `RunConfig`, `VM`, `RuntimeError` and its kinds, and the trace
  event/result types; and
- `CancellationToken` and resource-kind diagnostics.

The library accepts in-memory artifacts and programs. It does not read files,
print CLI trace lines, select process exit codes, or call `process::exit`.
The binary remains responsible for argument parsing, file-size limits and IO,
CLI rendering, and exit-code compatibility. It imports the same public modules
and re-exports used by library callers, so CLI execution still uses the same
verifier, linker, and VM implementation.

## Error and version boundary

This first slice preserves existing error contracts: artifact parsing and
verification return `format::ParseError`, linking returns its existing
deterministic `String` diagnostics, and execution returns
`Result<String, RuntimeError>`. The additive follow-up that introduces the
first typed linker/artifact facade and version constants is recorded in
[`vm-library-error-boundary-001.md`](vm-library-error-boundary-001.md); this
record remains the decision for the initial library extraction and its
compatibility-preserving starting point.

The crate remains `publish = false`, has no crates.io or SemVer promise, and
does not promise `Send`/`Sync`. `Rc`/`RefCell`, VM-borrowed programs, and
single-threaded cancellation semantics remain explicit constraints.

## Evidence and non-goals

`vm-rs/tests/library_api.rs` runs a formatted artifact through parse, verify,
run, and trace; links a module artifact and runs the linked program; and runs
two VM instances independently. The existing CLI, artifact, module, debugger,
and Rust golden tests remain the compatibility evidence.

This slice does not add persistent sessions, snapshot/rollback, host-retained
runtime values, plugin/native registration, process sandboxing, binary
artifacts, or a separate public profiling API.

```sh
cargo test --manifest-path vm-rs/Cargo.toml
```
