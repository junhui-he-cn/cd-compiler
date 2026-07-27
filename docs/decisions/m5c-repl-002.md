# M5C-REPL-002: explicit import search paths

Status: implemented as a prototype slice.

## Decision

Extend `tools/repl.py` with repeatable `--import-path DIR` options. Each
transcript replay forwards those paths to the production C++ compiler, so
non-explicit imports such as `import "math";` resolve through the same ordered
search-path rules as ordinary file-backed compilation. Imported public
definitions remain available to later accepted forms because the import stays
in the accepted transcript.

The existing commit/rollback and output-suffix rules remain unchanged. A
failed import form is rejected without adding its import to the transcript;
the next form is compiled against the last accepted state.

## Migration and compatibility

The wrapper remains source-backed and replays the production frontend and Rust
VM. No package manifest, import map, or second module resolver is introduced.
Explicit relative imports are still relative to the temporary session source,
so a stable project-root/session-root option is deferred. Search paths are
ordered in command-line order and can be repeated.

## Quantitative gate

`tests/repl_tests.py` supplies a temporary `math.cd` through `--import-path`,
uses its exported function across multiple forms, and verifies that a failed
`missing` import does not prevent a later independent form from succeeding.
The existing `repl` CTest and M0A inventory case remain the gate.

## Out of scope

- explicit relative-import project roots;
- package manifests and import maps;
- in-process module/session state;
- incremental bytecode or runtime-state reuse;
- persistent semantic caching.

## Old-path deletion condition

The wrapper's command-line search-path forwarding can move into a native
session service once that service consumes the same ordered import resolution,
visibility, failed-import rollback, and transcript corpus without a duplicate
module resolver.
