# SSA and optimization implementation plan

> This is a staged plan for the proposed `M7-IR-SSA-001` design. It is not an
> instruction to implement every stage in one change.

## Goal

Build a verified internal CFG/SSA pipeline, add opt-in O1 optimizations, and
preserve the existing IR/bytecode/Rust VM contracts. Keep O2 and local scalar
promotion behind explicit follow-up decisions.

## Sequence

Current progress: the CFG foundation, SSA structural-shell, deterministic
dominance-analysis, phi-placement, binding/effect-contract, and SSA
memory-slot rename and de-SSA copy-plan slices in steps 2-3 are implemented on
`feat/ssa-optimization-design`; the internal linear de-SSA layout is now also
implemented, as are the internal `IRFunction` and verified program-level
adapters. These slices plus the internal O1 copy/phi simplification and pure DCE service are
covered by `ctest.control_flow_graph`,
`ctest.ssa`, `ctest.dominance`, and `ctest.ssa_phi_placement`; the rename
coverage is part of `ctest.ssa`, and the binding contract is additionally
covered by `ctest.ssa_contract`, while the optimizer boundary is covered by
`ctest.optimizer`. Default ordinary-IR/CLI integration, CFG rewrites,
constant folding, and later work remain future implementation slices. The
constant-evaluation boundary is now frozen in `Optimizer` and covered by
`ctest.optimizer`: only finite, serializable primitive successes are foldable;
runtime traps and non-finite results are never silently folded. The O0
debug-local policy and schema-3 cache identity are now frozen and covered by
`ctest.module_cache` plus the existing module-cache matrix. The binding contract
is populated by
`IRCompiler` and covered by the existing `ir_source_location` integration
test. `optimizeIRFunction` now proves the internal one-stream adapter and
preserves ordinary parameter/binding metadata, source/offset maps, and
dependency anchors without physical register allocation.
`optimizeIRProgram` now traverses the anonymous main stream and all nested
functions in stable table order, while `SSADeSSAProgramResult::verify` and
`rebuild` protect function indices and copy program-level pools/tables without
connecting the adapter to CLI or artifact emission.

### 1. Freeze the baseline and internal contracts

Files:

- `docs/decisions/m7-ir-ssa-optimization-001.{md,json}`
- `docs/superpowers/specs/2026-07-30-ssa-optimization-design.md`
- `docs/roadmap.md`

Tasks:

- record the current branch/base commit and verification inventory;
- define `IRBinding`/storage metadata without changing emitted bytecode;
- define the effect/trap classification and optimizer fingerprint; and
- add no optimization behavior yet.

Gate: documentation review, JSON validity, `git diff --check`, and a clean
baseline build/test record.

### 2. Add CFG data structures and verifier

Files:

- create `include/ControlFlowGraph.hpp`, `src/ControlFlowGraph.cpp`;
- add CTest unit coverage under `tests/`; and
- update `CMakeLists.txt` only for the focused test target.

Tasks:

- split main/function streams deterministically;
- validate branch/terminator shapes and successor edges;
- preserve old instruction indices and module dependency anchors; and
- make malformed CFGs fail with compiler-internal diagnostics.

Gate: unit tests for straight line, diamond, loops, nested loops, unreachable
regions, invalid targets, and offset mapping.

### 3. Add SSA form, construction, verification, and de-SSA

Files:

- create `include/SSA.hpp`, `src/SSA.cpp`;
- extend `IR.hpp` only for explicit binding/storage metadata; and
- add SSA unit tests.

Tasks:

- implement `SSAValue`, `MemorySlot`, phi nodes, and terminators;
- compute dominators/frontiers and rename promotable slots;
- keep captured/module/unknown slots as explicit memory effects;
- verify dominance, phi incoming completeness, and instruction shapes; and
- lower phis with critical-edge splitting and parallel copies.

The admitted sub-slice currently covers value allocation, local-slot rename,
parameter initialization, phi incoming filling, non-promotable memory
preservation, dominance/edge/shape verification, and malformed-input
rejection. The current de-SSA slice plans ordered edge copies and cycle
temporaries, then materializes them into an internal linear layout with
critical-edge split blocks and offset maps, then adapts the result to existing
ordinary IR. Default-pipeline and bytecode integration remain follow-up
slices.

Gate: SSA verifier tests, loop backedge/diamond phi tests, copy-cycle tests,
and an O0 CFG/SSA/de-SSA semantic round trip with unchanged existing output.
The program-level result/rebuild boundary is covered by `ctest.optimizer`;
default-pipeline and bytecode integration remain follow-up slices.

### 4. Add the pass manager and opt-in O1

Files:

- create `include/Optimizer.hpp`, `src/Optimizer.cpp`;
- modify `src/main.cpp` for `--opt-level 0|1` and developer dumps;
- add focused optimization fixtures and runner coverage.

Tasks:

- make O0 the default (the current internal service already treats O0 as an
  identity result);
- implement reachability, jump/block simplification, constant/copy/phi
  propagation, safe folding, and limited DCE;
- run the verifier before and after each pass in debug/test builds; and
- report deterministic optimization counters outside artifacts.

The admitted sub-slice implements only the internal `optimizeSSA` boundary and
its verified per-stream/program adapters: copy propagation,
dominance-checked trivial-phi simplification, and pure dead-code removal. It
does not add `--opt-level`, invoke `IRCompiler`, rewrite CFG blocks, fold
constant values, or emit optimized artifacts.

Gate: O0 goldens remain unchanged; O1 branch/constant/copy cases show smaller
IR and exact C++/Rust output parity; runtime traps and evaluation order remain
unchanged.

### 5. Reuse artifact/debug/module boundaries

Files:

- `src/BytecodeCompiler.cpp` only if offset mapping needs an adapter;
- `include/ModuleCache.hpp`, `src/ModuleCache.cpp`, `src/main.cpp`;
- `BytecodeTextEmitter`/Rust files only if an existing contract needs a
  compatibility-preserving metadata update; and
- module artifact/cache/debug tests.

Tasks:

- remap dependency offsets after optimized module lowering (the internal
  one-stream adapter now exercises this map);
- include level/pipeline identity in module-product cache decisions (schema 3
  now records the O0 identity and rejects stale schema 2 manifests);
- retain valid debug source spans, keep source-visible runtime cells in the
  default O1 policy, and make O0 trace the stable debug path; and
- prove no SSA-only data leaks into `cdbc 0.1`.

Gate: cold/hit/stale module cache matrix, imports/re-exports, linked product
execution, artifact canonicalization, and debugger regression.

### 6. Decide and implement later O2 work separately

Candidate files:

- `src/SSA.cpp`, `src/Optimizer.cpp`;
- a liveness/register-allocation service; and
- debug-local metadata if optimized tracing is admitted.

Required decisions before coding:

- eligible non-captured local promotion and observable local materialization;
- alias/memory model for aggregate mutation and callbacks;
- physical register allocation/coalescing and register-pressure targets; and
- optimized trace contract.

O2 must not be silently enabled as part of O1 delivery.

## Final verification

For an admitted implementation slice, run the repository commands required by
`AGENTS.md`, including CMake/CTest, golden tests, artifact/module-cache/LSP/
debugger tests, Rust VM goldens, Cargo tests, canonical verification,
malformed/boundary checks, removal of `tests/__pycache__`, and
`git diff --check`. Record pre-existing failures separately from optimizer
failures.
