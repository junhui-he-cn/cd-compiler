# Unified Module Compilation Design (Multi-File Redesign)

Status: draft for review. User decision on 2026-08-10: the legacy
"no-import combined source / import-aware module graph" auto rule is removed.
The only supported multi-file model is one module per file.

## Purpose and scope

This design replaces the dual front-end path in `FrontendSession` with one
module-based pipeline. Every source file, every stdin input, and every virtual
LSP file is a module. The CLI file list is an ordered set of entry modules;
cross-file visibility requires `import` plus `export`. The combined-source
path, the `scanTokensUntil(TokenType::Import)` mode probe, double source
loading, and the `remapDirect*` diagnostic machinery are deleted.

This is an explicit compatibility-contract change. It supersedes the
"direct ordered multi-file compilation" contract in `docs/roadmap.md`,
`AGENTS.md`, and `README.md`, and the `m3a-graph-program-handoff` carve-out
that leaves the module graph empty for combined entry programs.

## Non-goals

- A `--combine` flag or a synthetic combined module.
- Package or logical module identity (deferred with project-root work).
- New `.cdbc`, `.cdi`, or `cdbc-cache` schemas; O0 default; Rust VM
  execution semantics; module-cache creation/repair policy (C4 stays gated).

## Target semantics

### Module identity and dependency graph

- Module identity remains the canonical path (normalized existing path),
  with canonical de-duplication and import-cycle rejection with a full chain.
- Every compilation produces a `ModuleGraph` whose nodes are modules and
  whose edges are `Import`/`ReExport` declarations. `Program.moduleGraph` is
  always present for file-backed, stdin, and virtual-input compilations.
- Each module is read, lexed, and parsed exactly once. Imports and
  source-bearing re-exports are discovered from parsed AST nodes.
- Multiple entry modules aggregate their own lexer/parser errors in CLI order
  before later stages run; import/cycle loading errors still stop immediately.

### CLI and stdin

- Every positional CLI file is an entry module, in command-line order.
  Duplicate canonical CLI files are deduplicated.
- Entry modules do not share a top-level scope. A declaration in one entry
  module is visible to another entry module only through `import`/`export`.
- Stdin is one pathless entry module. `import` from stdin keeps the existing
  rejection: `Import error: import is not supported from stdin`.
- LSP virtual files remain modules with open-over-disk precedence.

### Imports

- Relative imports resolve against the importing file's directory, then
  against `-I`/`--import-path` directories for non-explicit spellings.
  Existing search rules and `virtualImportRoots` behavior are unchanged.
- Preloaded `.cdi` sidecars, `cdbc-cache 0.2` manifests, strict/fallback
  modes, and module-product cache identity are unchanged.

### Checking and lowering

- Type checking runs in module dependency order; a failed module suppresses
  its importers and produces one located diagnostic per independent failed
  module. Interfaces precede dependent module bodies.
- `--emit-bytecode out.cdbc file...` emits a linked program whose entry
  module bodies execute in CLI order, with imported bodies inlined by the
  existing `IRCompiler::compile` entry-module path.
- `--emit-module-bytecode dir file...` emits one module product per graph
  node; the Rust VM `link` command combines them. Entry products keep
  contiguous entry order starting at zero in CLI order.
- Module cache keys keep their current components (identity, source hash,
  interface hash, optimization level and pipeline fingerprint, entry order,
  dependency hashes).

### Diagnostics and observability

- Every file-backed module diagnostic reports the original path and
  file-local line/column through the existing `DiagnosticSourceContext`
  path. The `remapDirect*` combined-source remapping layer is deleted.
- Compatibility carve-outs pending explicit decisions:
  - a single entry module (one CLI file, no imports) keeps pathless
    diagnostics;
  - stdin keeps pathless diagnostics;
  - AST text output keeps today's shapes: flat `Program` output for a
    single module, `Module N entry` wrappers for multi-module programs.
- `--tokens`, `--ir`, `--bytecode`, and `--module-interface` operate on
  entry modules in CLI order.
- The formatter continues to format each entry source file independently.

## Architecture changes

### FrontendSession

- `loadFiles(paths)`:
  1. deduplicate canonical entry paths;
  2. call `loadFile(path, isImport=false, isEntry=true, fileDiagnostics=true)`
     for each entry in CLI order;
  3. always `rebuildModuleGraph()`, `rebuildCombinedSource()`, and
     `assembleProgram()` in module shape.
- `loadStdin(input)` loads one pathless entry module and rejects parsed
  top-level imports.
- Delete: `directInputs_`, `directSourceLineStarts_`, `directDisplayTokens_`,
  `scanTokensUntil(Import)` probing, `remapDirectDiagnostic`,
  `remapDirectLexDiagnostics`, `remapDirectDiagnostics`, and the
  `hasImports_` branch in `assembleProgram()`.
- Keep: `ParsedUnit`, canonical de-duplication, cycle detection, sidecar
  preload, `ModuleGraph` rebuild, preloaded interface rebuild, and
  file-local diagnostic wrapping.

### Program and downstream

- `Program` always carries a `moduleGraph` and a `ModuleStmt` list.
- `main.cpp`: the formatter and all downstream consumers use the graph path
  unconditionally; the no-graph branches are removed.
- `IRCompiler::compile` keeps compiling entry modules in program order;
  `compileModule` and `IRModuleDependency` are unchanged.
- `writeModuleArtifacts` drops the "requires an import-aware module graph"
  precondition; any graph satisfies it.

## Fixture and documentation migration

- `tests/golden/multi_file_functions/` and
  `tests/bytecode_artifacts/multi_file_functions/` currently rely on shared
  top-level scope across two CLI files. Convert them to explicit module
  semantics (entry `main.cd` imports and uses `lib.cd` exports), then
  refresh `ast.out`, `ir.out`, `bytecode.out`, `run.out`, and `expected.cdbc`.
- Keep `args.txt` support in test runners; it now denotes ordered entry
  modules.
- Update `README.md`, `AGENTS.md`, `docs/compiler-developer-guide-zh.md`,
  `docs/roadmap.md`, and supersede the affected decision records
  (`m3a-graph-program-handoff`, `m3a-module-graph` text that reserves the
  combined path).
- Refresh `tests/verification_inventory.json` after fixture changes and
  review the generated case metadata.

## New focused coverage

- Two entry modules without imports cannot see each other's top-level
  declarations (type error), proving module isolation.
- Two entry modules with `import`/`export` run entry bodies in CLI order.
- Diamond dependency shared by two entries: one parse per module, canonical
  de-duplication, and stable entry order.
- Multi-entry `--emit-bytecode` linked output vs multi-entry
  `--emit-module-bytecode` + Rust `link` parity.
- Diagnostics: pathful for multi-entry programs, pathless for the single
  entry carve-out, stdin import rejection unchanged.
- Module cache round trip on a multi-entry module graph.

## Open decisions (recommendations)

- **D-AST**: keep today's text surfaces (flat single-module print, module
  wrappers for multi-module) to avoid corpus churn. Recommended.
- **D-DIAG**: keep the single-entry pathless diagnostic carve-out.
  Recommended for now; revisit with tooling work.
- **D-ARTIFACT**: multi-entry `--emit-bytecode` emits a linked program in
  CLI order rather than being rejected. Recommended; matches the existing
  compiler behavior.
- **D-ID**: canonical path remains module identity; package/logical identity
  is deferred.

## Phases

1. **P0 - decisions**: record this spec and the D-* decisions as decision
   records; update roadmap status after approval.
2. **P1 - front-end unification**: implement the module-only
   `FrontendSession` path, delete combined-source machinery, migrate the two
   `multi_file_functions` fixtures, add the focused coverage above, and
   refresh the verification inventory.
3. **P2 - documentation**: update README, AGENTS, developer guide, roadmap,
   and supersede obsolete decision-record text.

## Verification gate

Full repository verification must pass after P1/P2:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
python3 tests/verification_inventory.py
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
python3 tests/run_boundary_tests.py ./build/compiler_design
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs --report build/malformed-report.json
python3 tests/run_golden_tests.py ./build/compiler_design
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
python3 tests/lsp_tests.py ./build/compiler_design
python3 tests/debugger_tests.py ./build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
cargo test --manifest-path vm-rs/Cargo.toml
rm -rf tests/__pycache__
git diff --check
```
