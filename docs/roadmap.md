# Compiler Design Roadmap

This is the active execution plan for the compiler, language, and compiler
tools. The Rust runtime has a separate plan in
[`docs/vm-roadmap.md`](vm-roadmap.md). Work that changes the `.cdbc` boundary,
native calls, debug metadata, module products, or compatibility policy is a
joint compiler/VM slice and must satisfy both roadmaps.

The baseline was audited on 2026-08-02 at `master` commit `61cbbc79`. Code,
tests, decision records, and Git history are the completion evidence; this
document intentionally does not repeat the implementation history of every
finished slice.

## 0. Current phase: consolidation only

As of 2026-08-10, the default queue accepts consolidation work on existing
functionality only: semantic-soundness repairs, correctness fixes, internal
refactors, performance optimization, and evidence or documentation audits.
New language syntax, new tooling or CLI features, new runtime capabilities,
new artifact or host surfaces, and concurrency/language expansion are not in
the default queue until an explicit phase decision reopens them. Existing
compatibility contracts (`cdbc 0.1`, O0 default, source fallback, C++/Rust
parity, interpreter-default VM execution) remain unchanged.

On 2026-08-10 the user explicitly reopened one compatibility boundary:
multi-file compilation moves to the one-module-per-file model and the legacy
combined-source path is removed (see C6 and
[`2026-08-10-unified-module-compilation-design.md`](superpowers/specs/2026-08-10-unified-module-compilation-design.md)).
C6 P1 and P2 landed through 2026-08-12 (`cb8b7aac`): the module-only
`FrontendSession` path, migrated fixtures, documentation, and decision-record
updates are complete.

## 1. Planning rules

Roadmap items use four states:

- **active**: the next independently deliverable slice;
- **queued**: ordered work whose prerequisite is already understood;
- **decision gate**: implementation must stop until the policy is explicitly
  chosen;
- **deferred**: not in the default development queue.

Every implementation slice must define its deliverable, non-goals, migration
or compatibility boundary, named tests, and deletion condition. Only one
slice per track should be active at a time. A completed item moves to a
decision record or Git history instead of accumulating in this file.

`cdbc 0.1`, per-file module compilation with CLI entry order, O0 as the
default optimizer level, and source fallback for cold or repairable
module-product builds remain compatibility contracts until an explicit
decision changes them. The previous "direct ordered multi-file compilation"
contract is superseded by C6.

## 2. Current baseline

| Area | Shipped baseline | Open boundary |
| --- | --- | --- |
| Verification | Versioned 1,794-case inventory, canonical runner, boundary and malformed corpora | Refresh inventory metadata only when cases or CTest checks change |
| Front end | Typed source identities, lossless source, declaration/semantic indexes, import-aware module graph | Remove legacy paths only when all consumers use the indexed services |
| Language | Functions, closures, generics and constraints, `optional<T>`, enums/patterns, named and recursive structs, collection semantics, `Eq`/`Ord`/`Hash` capabilities | New syntax is demand-driven; semantic soundness work has priority |
| Modules | Public interfaces, `.cdi`, independent module products, linker inputs, `cdbc-cache 0.2`, strict and fallback modes | Cache creation/repair policy is not yet a default-strict contract |
| IR and optimization | Linear register IR plus verified CFG/SSA/de-SSA and explicit `--opt-level 1` | O0 stays default; register-allocation and default-level policy remain open |
| Tools | Formatter, open/closed-workspace LSP definition and references, trace and interactive VM debugger | LSP completion, workspace rename, and incremental sessions are deferred |
| Artifact boundary | Compiler emits validated linked and module `cdbc 0.1` products with debug metadata | No successor format has been justified |

The authoritative implementation contracts remain in `README.md`,
`docs/language-grammar.ebnf`, `docs/bytecode-text-format.md`, and
`docs/decisions/`.

## 3. Joint prerequisite

### X1: Compiler/VM compatibility matrix

**Status:** completed on 2026-08-02. Decision record:
[`x1-compiler-vm-compatibility-001.md`](decisions/x1-compiler-vm-compatibility-001.md).

**Outcome:** add one concise compatibility record covering compiler version,
`cdbc` version, linked versus module products, metadata-free versus debug
artifacts, module-cache manifest schema, native-name contract, and Rust VM
library/CLI versions. Record which combinations execute, are upgraded, or are
rejected.

The shipped matrix validates seven cells against source constants, the native
registry, inventory revision, and evidence paths. It is implemented by
`tests/vm_compatibility_matrix.py` and its selftest; the refreshed artifact
audit now covers 126 assertions across 63 fixtures.

**Boundary:** this slice documents and tests the existing contract. It does not
introduce `cdbc 0.2`, a binary format, versioned native IDs, or a new cache
schema.

**Gate:** the matrix must point to existing positive and rejection fixtures;
missing combinations become focused tests registered in the verification
inventory. C++ emit, Rust parse/dump/link/run, metadata-free compatibility,
module products, cache identity, and malformed-version rejection must agree.

**Why first:** every later optimizer, runtime-lifetime, host-API, and artifact
decision needs a stable statement of the boundary it must preserve.

## 4. Compiler queue

### C1: Make O1 ready for a default-policy decision

**Priority:** P0. **Status:** C1 complete on 2026-08-03; O0 remains the compatibility default.

The existing explicit O1 pipeline is useful but not yet a default replacement
for O0. Finish the evidence in three narrow slices:

1. **C1A - optimized debug contract (implemented):** define which source
   locations, trace events, frame locals, and runtime cells must remain
   observable at O1. Add O0/O1 trace and debugger parity cases for branches,
   loops, closures, imported functions, eliminated values, and runtime failures.
   See [`c1a-optimized-debug-001.md`](decisions/c1a-optimized-debug-001.md).
2. **C1B - register policy (implemented):** measure CFG-aware peak
   virtual-register pressure on the checked-in O0/O1 workload matrix and
   decide whether O1 retains virtual registers or needs a separate physical
   allocation/coalescing stage. O1 retains virtual registers for now; see
   [`c1b-register-policy-001.md`](decisions/c1b-register-policy-001.md).
   Do not add an O2 allocator until the mapping and spill/debug rules are
   written.
3. **C1C - default-level decision (implemented):** compare the semantic,
   artifact-size, compile-time, runtime, cache, and debugger evidence. Keep O0
   as the compatibility default and retain explicit O1 opt-in; see
   [`c1c-default-level-001.md`](decisions/c1c-default-level-001.md).

**Decision gate:** changing the default optimization level is a user-visible
pipeline decision. Stop after presenting the C1 evidence; do not switch the
default automatically.

**Gate:** optimizer unit/CLI cases, all `optimizer.*` inventory cases, O0/O1
output/error/exit parity, module-cache identity, source/debug mapping, the
checked-in workload comparison report, canonical verification, and Rust VM
execution.

### C2: Close semantic-soundness gaps one rule at a time

**Priority:** P1. **Status:** superseded on 2026-08-13 by the explicit
optional-unwrapping design
(`2026-08-13-explicit-optional-unwrapping-design.md`). The nullable
flow-analysis machinery this item repaired (nil-check narrowing, alias-aware
field/index facts, dynamic index targets, and loop-exit facts) was removed;
`optional<T>` is unwrapped explicitly with `if let`, `while let`, `?`, `??`,
and `match` binding arms. Semantic-soundness work resumes only with a concrete
proposal against the current language surface.

### C6: Unify multi-file compilation on the per-file module model

**Priority:** P1. **Status:** complete on 2026-08-12. Both P1 and P2 landed:
the module-only `FrontendSession` path, the `multi_file_functions` fixture
migration, module-isolation/multi-entry-order/artifact parity coverage, the
inventory refresh, and the README/AGENTS/developer-guide/superseded-decision
updates are all in place through `cb8b7aac`.

The legacy auto rule ("no import -> combined source, import -> module graph")
is removed. Every CLI file, stdin input, and LSP virtual file is an
independent module; the CLI file list is an ordered set of entry modules, and
cross-file visibility requires `import` plus `export`. The combined-source
path, the `scanTokensUntil(Import)` mode probe, double source loading, and the
`remapDirect*` diagnostic machinery are deleted. `Program` always carries a
module graph, and `--emit-bytecode` emits a linked program whose entry module
bodies execute in CLI order.

See the full design in
[`2026-08-10-unified-module-compilation-design.md`](superpowers/specs/2026-08-10-unified-module-compilation-design.md).

**Deliverables:** P1 implements the module-only `FrontendSession` path,
migrates the `multi_file_functions` fixtures to module semantics, adds
module-isolation, multi-entry order, and multi-entry artifact parity coverage,
and refreshes the verification inventory. P2 updates README, AGENTS, the
developer guide, and superseded decision records.

**Gate:** full verification must pass with the exact commands in the spec;
`multi_file_functions` goldens and `module-interface` goldens are refreshed
only where the new uniform module semantics intentionally changes output.

### C3: Complete closed-workspace language tooling

**Priority:** P1. **Status:** deferred by user direction on 2026-08-09. See
[`m5b-lsp-deferred-001.md`](decisions/m5b-lsp-deferred-001.md).

The shipped closed-module definition/reference boundary remains supported. C3A
through C3C are outside the default queue and resume only after explicit
reprioritization or a concrete editor/workspace consumer. When resumed, proceed
in this order:

1. **C3A - closed-module completion:** reuse `FrontendSession`, module graph,
   interfaces, and open-document precedence; do not add an LSP-only resolver.
2. **C3B - workspace rename:** produce deterministic edits for open and closed
   files inside declared workspace roots, with conflict and stale-document
   rejection.
3. **C3C - incremental workspace analysis:** introduce caching only after
   correctness and invalidation identities are explicit and measured.

Completion and rename are separate slices. Unknown or dynamic receivers,
cross-workspace edits, file renames, and persistent on-disk LSP caches stay out
of C3A.

### C4: Decide module-product creation and repair policy

**Priority:** P2. **Status:** complete on 2026-08-13. Decision record:
[`c4-module-product-creation-repair-001.md`](decisions/c4-module-product-creation-repair-001.md).

Module-product builds are strict by default: cold builds bootstrap from
source, an invalid or inconsistent manifest requires explicit repair
(`--module-cache-fallback` or a cache-directory reset), normal source and
dependency drift rebuilds transparently, and schema-4 product content digests
detect corrupted cached products before reuse. Interface-only consumers keep
their strict sidecar trust boundary, and `--module-cache-fallback` is now
valid for both modes. Offline product-only builds remain unsupported and entry
modules always compile from source.

### C5: Reassess incremental sessions and REPL

**Priority:** P2. **Status:** deferred.

Re-audit the old branch prototype against current module interfaces, cache
identity, formatter, diagnostics, and LSP workspace state. First extract an
incremental compiler-session API with deterministic invalidation; only then
decide whether an interactive REPL is a supported product. Branch-only code is
not baseline behavior.

## 5. Deferred compiler work

The following items require new evidence or an explicit product decision and
are not in the default queue:

- O2 physical register allocation, broad CFG rewriting, cross-module
  optimization, or serialized SSA;
- a default O1 switch before C1 completes;
- new syntax, operator families, or capability systems without a concrete
  library or language use case;
- offline product-only builds that do not require dependency source files;
- a successor or binary artifact format before load-time/size/integrity data
  justifies it;
- JIT, async language semantics, persistent runtime sessions, or compiler-owned
  concrete data-structure implementations.
- closed-workspace LSP completion, workspace rename, and incremental analysis
  until C3 is explicitly resumed.

## 6. Dependency order

```text
completed X1 compatibility matrix
  -> C1A optimized debug contract
  -> C1B register policy
  -> C1C default-level decision
  -> optional O2 design only if evidence requires it

completed C6 design decision (2026-08-10)
  -> C6 P1 module-only front-end unification (complete)
  -> C6 P2 documentation and decision-record updates (complete)

completed C2 (superseded by explicit optional unwrapping)

shipped closed-module definition/references
  -> C3A closed completion
  -> C3B workspace rename
  -> C3C measured incremental analysis

X1 + shipped module cache
  -> C4 strict-by-default creation/repair policy (complete)

C3/C4 stable boundaries
  -> C5 incremental session re-audit
```

C1, C2, C4, and C6 are complete. There is no active compiler slice in the
default queue; C3 is deferred and C5 remains deferred.

## 7. Verification contract

Rebuild before running source-backed tests. The full repository gate remains
the command set in `AGENTS.md`; the canonical minimum is:

```sh
cmake -S . -B build
cmake --build build
python3 tests/verification_inventory.py
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
python3 tests/run_boundary_tests.py ./build/compiler_design
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs --report build/malformed-report.json
ctest --test-dir build --output-on-failure
cargo test --manifest-path vm-rs/Cargo.toml
rm -rf tests/__pycache__
git diff --check
```

Add or refresh inventory metadata only when fixtures or CTest checks change,
then review the generated case IDs. Focused development checks may run first,
but a slice is not complete until its named focused cases and all affected
cross-boundary gates pass.

## 8. Completion rule

The roadmap is working when the active queue stays short, completed work leaves
this file, compatibility boundaries are explicit, and every default or format
change stops at a visible decision gate. More feature names in the document are
not progress; verified removal of uncertainty is.
