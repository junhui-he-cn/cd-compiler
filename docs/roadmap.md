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

`cdbc 0.1`, direct ordered multi-file compilation, O0 as the default optimizer
level, and source fallback for cold or repairable module-product builds remain
compatibility contracts until an explicit decision changes them.

## 2. Current baseline

| Area | Shipped baseline | Open boundary |
| --- | --- | --- |
| Verification | Versioned 1,941-case inventory, canonical runner, boundary and malformed corpora | Refresh inventory metadata only when cases or CTest checks change |
| Front end | Typed source identities, lossless source, declaration/semantic indexes, import-aware module graph | Remove legacy paths only when all consumers use the indexed services |
| Language | Functions, closures, generics and constraints, `optional<T>`, enums/patterns, named and recursive structs, collection semantics, `Eq`/`Ord`/`Hash` capabilities | New syntax is demand-driven; semantic soundness work has priority |
| Modules | Public interfaces, `.cdi`, independent module products, linker inputs, `cdbc-cache 0.2`, strict and fallback modes | Cache creation/repair policy is not yet a default-strict contract |
| IR and optimization | Linear register IR plus verified CFG/SSA/de-SSA and explicit `--opt-level 1` | O0 stays default; register-allocation and default-level policy remain open |
| Tools | Formatter, open/closed-workspace LSP definition and references, trace and interactive VM debugger | Closed-module completion/rename and incremental sessions remain open |
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

**Priority:** P0. **Status:** C1A implemented on 2026-08-03; C1B is next.

The existing explicit O1 pipeline is useful but not yet a default replacement
for O0. Finish the evidence in three narrow slices:

1. **C1A - optimized debug contract (implemented):** define which source
   locations, trace events, frame locals, and runtime cells must remain
   observable at O1. Add O0/O1 trace and debugger parity cases for branches,
   loops, closures, imported functions, eliminated values, and runtime failures.
   See [`c1a-optimized-debug-001.md`](decisions/c1a-optimized-debug-001.md).
2. **C1B - register policy:** measure virtual-register pressure on the checked-in
   O0/O1 workload matrix and decide whether O1 retains virtual registers or
   needs a separate physical allocation/coalescing stage. Do not add an O2
   allocator until the mapping and spill/debug rules are written.
3. **C1C - default-level decision:** compare the semantic, artifact-size,
   compile-time, runtime, cache, and debugger evidence. Choose either to keep
   O0 as the compatibility default or make O1 the default with an explicit
   migration note.

**Decision gate:** changing the default optimization level is a user-visible
pipeline decision. Stop after presenting the C1 evidence; do not switch the
default automatically.

**Gate:** optimizer unit/CLI cases, all `optimizer.*` inventory cases, O0/O1
output/error/exit parity, module-cache identity, source/debug mapping, the
11-workload comparison report, canonical verification, and Rust VM execution.

### C2: Close semantic-soundness gaps one rule at a time

**Priority:** P1. **Status:** queued; may proceed independently of C1 when it
does not change IR or artifact behavior.

Start with a decision/corpus slice for exactly one conservative nullable-flow
case: alias-aware field/index facts, dynamic index targets, or normal `for-in`
exit facts. Specify the alias and mutation model, invalidation, loop exit proof,
and diagnostic compatibility before implementation.

Each implementation slice must include positive, negative, mutation, alias,
nested-loop, closure/callback, and import cases. Unsupported cases remain
conservative. Do not replace the whole flow engine or accept a case merely
because the current corpus lacks a counterexample.

### C3: Complete closed-workspace language tooling

**Priority:** P1. **Status:** queued after the shipped closed-module
definition/reference boundary.

Proceed in this order:

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

**Priority:** P2. **Status:** decision gate, not implementation-ready.

Define cold build, missing product, malformed product, source-only change,
public-interface change, dependency change, offline build, and explicit repair
behavior for linked and independent module-product paths. Keep current safe
fallback until the matrix and migration are approved. A strict-by-default
change must not be smuggled in as cache cleanup.

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
- removing source fallback from module creation/repair;
- a successor or binary artifact format before load-time/size/integrity data
  justifies it;
- JIT, async language semantics, persistent runtime sessions, or compiler-owned
  concrete data-structure implementations.

## 6. Dependency order

```text
completed X1 compatibility matrix
  -> C1A optimized debug contract
  -> C1B register policy
  -> C1C default-level decision
  -> optional O2 design only if evidence requires it

shipped semantic index + flow facts
  -> C2 one soundness decision/corpus
  -> C2 one admitted implementation slice

shipped closed-module definition/references
  -> C3A closed completion
  -> C3B workspace rename
  -> C3C measured incremental analysis

X1 + shipped module cache
  -> C4 creation/repair policy decision

C3/C4 stable boundaries
  -> C5 incremental session re-audit
```

X1 is complete. The recommended next compiler slice is C1A. C2 and C3 are valid
secondary tracks, but should not be mixed into the optimizer slice.

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
