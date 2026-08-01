# SSA and optimization pipeline design

Date: 2026-07-30

Status: design proposal with admitted internal SSA rename, de-SSA copy-plan,
linear-layout, ordinary-IR adapter, conservative internal O1
value-simplification, and explicit O1 CLI/module-product/cache slices;
the CFG foundation, SSA structural shell, deterministic dominance analysis,
phi placement, binding/effect contract, rename slice, edge-copy plan, and
internal linear layout/adapter plus the one-stream optimizer boundary are
implemented on the feature branch. The
internal O1 slice covers proven-safe primitive constant folding, copy/phi
simplification, pure dead-code removal, block-preserving known-condition branch
normalization, post-de-SSA unreachable-block pruning, redundant
fallthrough-jump removal, and threading through jump-only blocks; explicit O1
lowering now invokes the verified program-level boundary. General block
merging/jump threading across non-empty blocks, default pipeline integration,
and broader optimization remain unadmitted. This document expands
[`M7-IR-SSA-001`](../../decisions/m7-ir-ssa-optimization-001.md); it does not
authorize implementation by itself.

## Goal

Introduce compiler optimization and SSA concepts without changing the current
language semantics or the `cdbc 0.1` artifact/VM boundary. The first useful
optimization should be measurable, opt-in, and easy to disable when debugging
or comparing golden output.

## Current architecture

The production path is:

```text
source
  -> FrontendSession / parser / AST
  -> TypeChecker + DeclarationIndex
  -> IRCompiler
  -> linear register IR (`IRProgram`)
  -> BytecodeCompiler
  -> cdbc 0.1
  -> Rust VM
```

`IRProgram` has explicit virtual registers, but each main/function body is
still a linear instruction vector. Branches identify instruction offsets and
there is no explicit CFG, dominance relation, or phi representation.

`LoadVar`, `StoreVar`, and `AssignVar` are not ordinary SSA assignments. They
access runtime cells used by globals, function locals, and closures. `Field`,
`Index`, their assignment forms, calls, native calls, and callbacks can observe
or mutate shared values. Imported module products additionally carry dependency
markers at instruction offsets. Every one of these facts constrains legal
motion and deletion.

The existing bytecode compiler is intentionally mechanical: it maps each IR
operation to the corresponding bytecode operation and preserves source spans.
The Rust VM owns runtime behavior and currently exposes deterministic source
trace events with runtime-cell locals.

## Target pipeline

```text
IRProgram
  -> CFGIR
  -> SSAIR + verifier
  -> PassManager(O0/O1/...)
  -> de-SSA / edge-copy lowering
  -> IRProgram-compatible virtual-register stream
  -> BytecodeCompiler
```

There are three important compatibility rules:

1. The front end remains the only owner of source semantics and binding
   resolution.
2. SSA is an internal form. Phi nodes, block IDs, and optimizer statistics do
   not enter `.cdbc`.
3. The optimizer returns ordinary IR plus remapped metadata, so existing
   bytecode, linking, Rust execution, and artifact validation can be reused.

## Internal data model

The planned internal model is intentionally smaller than a new language HIR:

```text
BlockId       stable index in one main/function body
SSAValue      single-definition value ID
MemorySlot    explicit runtime binding/storage ID
Phi           result + [(predecessor BlockId, SSAValue)]
SSABlock      phis + effect-ordered instructions + terminator
SSAFunction   entry block + blocks + value/slot metadata
```

Each instruction retains:

- its original opcode or a small internal equivalent;
- SSA value operands and optional result;
- side-effect/trap classification;
- the original instruction index for diagnostics and offset remapping; and
- an optional source span.

`MemorySlot` metadata is emitted once by the IR lowering boundary. It records
the resolved binding identity and storage class, not just the display name.
The initial storage classes are `local`, `captured`, `module`, `exported`, and
`synthetic`. Unknown or missing metadata is conservatively treated as
non-promotable memory.

The current contract represents this boundary with `IRBinding`: a
snapshot-local `BindingId`, resolved name, and explicit storage class. An
`IRInstruction` may carry the same binding ID for `LoadVar`, `StoreVar`, or
`AssignVar`, while the existing name operand remains unchanged for debugging
and bytecode lowering. `IRProgram::bindings()` owns one canonical record per
snapshot binding; each `IRFunction` keeps only the bindings visible in that
body, so a captured binding may be referenced by multiple functions without
duplicating its identity. The current lowering boundary populates source
declarations, parameters, captures, module/exported bindings, and compiler
synthetic cells. Missing identities, including interface-only imported values,
remain untagged and conservative. Only `Local` is eligible for promotion.

The effect contract is a conservative `IREffectSummary`. It records memory
reads/writes, potential traps, allocation, calls, observable output, and
control flow. The summary is analysis metadata only; it does not authorize
motion, deletion, folding, or artifact changes.

## CFG construction

For each main stream and function body:

1. collect entry `0`, every valid jump target, and the instruction immediately
   following each conditional/unconditional jump or return;
2. split the linear stream at those boundaries;
3. resolve successors from terminators and explicit fallthrough;
4. discard only blocks proven unreachable by the CFG pass, never source
   statements that might produce a runtime error before a branch; and
5. assign deterministic block IDs in original instruction order.

The builder must reject malformed jumps instead of silently repairing them.
The independent module dependency markers are attached to the nearest ordered
block/offset anchor and are included in the remapping table.

## Dominance analysis slice

The current branch computes sorted dominator sets, immediate-dominator tree
children, and dominance frontiers for the reachable CFG subgraph. Unreachable
blocks retain empty dominance metadata so later reachability passes can remove
them without inventing definitions. A reachable synthetic exit is treated as
an ordinary CFG node; predecessor edges from unreachable blocks do not
contribute to its or any other frontier. The result is verified against CFG
reachability, idom-tree, and frontier invariants before it is exposed to later
SSA construction.

## Phi placement slice

The current branch applies iterated dominance-frontier placement to
block-level definition sites supplied by a future IR lowering boundary. Only
`Local` memory slots are eligible. Definitions from unreachable blocks are
ignored, synthetic exit blocks are never placement targets, and returned
placements are sorted by slot ID and block ID. The placement function remains
metadata-only; value allocation, incoming-value filling, and renaming are
performed by the admitted construction slice below.

## SSA memory-slot rename slice

`renamePromotableMemorySlots` now consumes a register-form `SSAFunction` plus
the verified CFG and dominance tree. It allocates deterministic SSA values for
parameters, expression results, and local-slot phis; walks the dominator tree;
replaces promotable local loads with the current slot value; removes local
stores/assignments while pushing their values; and fills phi incoming values
in CFG predecessor order. Parameter-backed local slots seed the initial
memory stack, so a join can merge an initial parameter value with a branch
definition or loop backedge.

Captured, module, exported, synthetic, and unknown slots remain explicit
memory operations. Raw virtual-register definitions must be unique, and the
rename boundary rejects invalid slots, undefined local slots, malformed
variable operations, and uses whose raw definition does not dominate the use.
Unreachable blocks are renamed linearly for a complete internal shape but do
not contribute definitions or phi inputs. The result is verified before it is
returned; verification includes reachable-use dominance, same-block definition
order, phi-edge availability, and basic opcode operand shape. This remains an
internal service: it is not connected to the default `IRCompiler`, CLI,
bytecode, or module-product path.

The branch also exposes `planSSADeSSACopies`, which converts each phi's
incoming edge into a deterministic sequential move bundle. Identity moves are
removed, cycles receive one fresh temporary per cycle, and critical edges are
marked for a later split. This plan does not mutate the CFG, rewrite linear
jump offsets, or serialize any copy metadata; those responsibilities remain
part of the next lowering boundary.

`lowerSSADeSSACopies` materializes the plan into an internal linear result.
Non-critical copies use a unique successor entry or predecessor exit; critical
fallthrough edges receive an in-place split block, and critical branch-target
edges receive deterministic split blocks before the synthetic exit. The result
rewrites block-entry branch targets and carries original-instruction,
insertion-boundary, and module-dependency offset maps. It does not yet convert
the result to a whole `IRProgram` or connect to bytecode, debug-local, or cache
identity paths.

`lowerSSADeSSAToIR` is the internal ordinary-IR adapter. It keeps SSA value IDs
as virtual-register indices, computes `registerCount`, forwards parameter names
and binding metadata supplied by the caller, and preserves source spans,
synthetic-copy provenance, and offset maps. The explicit O1 program caller
invokes the verified program-level wrapper around this adapter; O0 continues
to bypass it for compatibility.

## Internal O1 value-simplification slice

`optimizeSSA` is the first internal pass-manager boundary. O0 verifies and
returns the input unchanged. O1 currently runs three deterministic passes:

1. propagate `Copy` values and simplify a phi only when all incoming values
   resolve to one value whose definition dominates the join;
2. fold a unary or binary expression when all operands are known finite,
   serializable primitives and the constant evaluator proves the operation
   succeeds; and
3. remove unused result-producing instructions only when `irEffectSummary`
   marks them pure. The current effect table therefore permits removal of
   unused `Constant` and `Copy` values only.

The result reports copy, phi, constant-fold, and instruction-removal counters;
the service
also exposes a stable diagnostic fingerprint. It re-verifies the SSA function
after each pass, retains all memory/call/allocation/trap/control-flow
operations, and does not alter the CFG, de-SSA offsets, ordinary IR, bytecode,
or cache keys inside the internal service. The explicit O1 program adapter
materializes newly folded primitive values in the existing constant pool
before ordinary IR/bytecode lowering. `ctest.optimizer` and `optimizer_cli`
are the focused verification cases. Known-condition branch normalization runs
on verified ordinary IR after de-SSA, converting only known primitive
conditional jumps to ordinary jumps; a rebuilt CFG then removes only blocks
proven unreachable and remaps retained source/insertion and dependency offsets;
it also removes only unconditional jumps targeting the next retained
instruction. A jump may thread only through a block containing one
unconditional jump. This placement preserves critical-edge copy validity.
General block merging/jump threading across non-empty blocks and default O0
replacement remain later decisions.

## SSA construction

The first construction algorithm is the standard dominance-frontier approach:

1. identify writes to each promotable `MemorySlot`;
2. compute dominators and dominance frontiers;
3. place one phi at each required frontier join;
4. rename definitions and uses along the dominator tree; and
5. fill phi incoming edges by predecessor block.

Pure expression temporaries already have one defining IR instruction. They are
lifted as direct SSA values. Non-promotable memory operations stay explicit:

```text
load_var captured_name  -> effectful value-producing instruction
assign_var captured_name, value -> effectful memory write
call/native_call         -> effect barrier
field/index assignment   -> aggregate memory write
```

No Memory SSA is required for the first release. This intentionally leaves
alias-sensitive optimizations conservative. A future alias model can add
memory tokens without changing the source language, but it must be a separate
decision.

## Effect and trap model

Optimization legality depends on more than whether an instruction produces a
value. The initial table is:

| Class | Initial treatment |
| --- | --- |
| Constant/copy with no allocation or trap | movable within a block and removable when unused |
| Known-safe primitive fold | fold only when static values prove success and result exactly matches runtime |
| Arithmetic/assertion/index/field read | potentially trapping; retain source order unless proven safe |
| Array/map/struct/variant/function creation | retain conservatively; allocation and identity effects are not yet specified |
| Load/store/assign variable | memory effect; preserve for non-promotable slots |
| Field/index assignment | aggregate memory effect; no first-slice alias elimination |
| Call/native/callback/print | observable effect barrier |
| Jump/conditional/return | control-flow effect; simplify only with proven target/condition facts |

The `irEffectSummary` implementation follows this table conservatively: only
constant and copy operations are currently pure; calls, aggregate accesses,
assertions, arithmetic, and comparisons retain a potential-trap or effect
classification until a later semantic proof narrows it.

In particular, constant folding must not turn a runtime division-by-zero or
dynamic type failure into a compile-time failure, nor move a failing index or
field access across a print, assignment, or call. A fold is allowed only when
the operation is known to succeed and has the same value representation.
The internal constant-evaluation boundary now makes that rule machine-checkable:
`Folded` is reserved for finite, cdbc-serializable primitive results;
`RuntimeTrap` covers division by zero and known operand type failures;
`NonFinite` covers values rejected by the current artifact constant contract;
and `Unsupported` covers non-primitive values or opcodes outside the admitted
subset. The O1 constant pass consumes this legality result; it never rewrites
an operation classified as a runtime trap or non-finite.

## Pass pipeline

### O0

Build and verify CFG/SSA, then immediately de-SSA. O0 is the compatibility
path and should preserve the current operation order, source spans, dependency
anchors, and bytecode text as far as the internal round trip permits. If the
round trip cannot preserve a byte-for-byte dump for a construct, the original
linear path remains the O0 implementation until that difference is explained.

### O1

The full O1 design is still broader than the admitted internal slice. The
current branch implements items 1-5 below in a constrained post-de-SSA form and
the local fallthrough/jump-only threading cleanup; general block merging and
threading across non-empty blocks remain planned:

1. known-condition jump normalization followed by unreachable-block pruning,
   redundant next-block jump removal, and jump-only-block threading;
2. constant propagation for proven non-trapping values;
3. constant folding for the approved primitive subset;
4. copy/phi simplification;
5. local dead-code elimination for non-trapping pure instructions; and
6. block merge and final jump threading.

`optimizeIRFunction` is the admitted internal adapter for one ordinary
`IRFunction`: it lifts one stream, runs the selected optimizer, and lowers it
back while preserving the ordinary signature and dependency/source offset
metadata. `optimizeIRProgram` now applies the same boundary to the anonymous
main stream and every function-table entry in stable index order. Its verified
`SSADeSSAProgramResult` preserves `MakeFunction` references and stream
metadata, and its explicit `rebuild` copies the name/source/binding tables and
interns folded values in the constant pool before replacing streams. The explicit O1 caller then reuses the
existing bytecode/artifact emitters; neither adapter invokes `IRCompiler` or
allocates physical registers.

Every pass has an input/output verifier boundary in debug builds and reports
small counters such as blocks removed, folds, copies removed, and instructions
deleted. Counters are diagnostics, not artifact content.

### Later O2 work

O2 is reserved for a later slice. Candidate work includes promotion of eligible
non-captured locals, liveness analysis, copy coalescing, linear-scan register
allocation, and carefully bounded loop passes. It cannot be admitted until the
debug-local policy and module-cache identity are resolved.

Inlining, LICM, global value numbering across effect barriers, speculative
devirtualization, vectorization, and cross-module optimization are outside
this proposal.

## De-SSA and lowering

For every phi, insert a parallel copy bundle on each incoming edge. The current
branch plans these bundles and resolves cycles with one fresh temporary virtual
register per cycle, then materializes them into an internal linear layout with
critical-edge split blocks and remapped branch/dependency offsets. A later
default-pipeline boundary must invoke the ordinary-IR adapter, preserve debug
locations, and remove redundant copies after any admitted optimization.

The lowering result uses existing `IRRegister`, `IRInstruction`, and `IROp`
types. It carries an old-to-new instruction offset map for:

- branch targets;
- `IRModuleDependency::instructionOffset` markers;
- source debug locations/ranges; and
- any future source-level local table.

No `phi`, `block`, `SSAValue`, or optimizer-only opcode is serialized into
bytecode or parsed by `vm-rs`.

## Debugging and source mapping

Every surviving instruction keeps an origin span. A folded expression points
at the source expression range; edge copies are hidden/internal unless an
existing location is safe to reuse. Removing a pure instruction may remove its
instruction-level trace event at O1.

The selected policy keeps the trace-local boundary conservative: O1 retains
runtime-cell operations for source-visible bindings, and local-slot SSA
promotion remains an internal experiment rather than a default-pipeline pass.
O1 may still omit trace events for removed pure instructions, so event-count
equivalence is not promised. O0 remains the stable debug path until a later
optimized trace mapping is separately admitted.

## Module/cache boundary

Each independent module product runs the same optimizer configuration
independently. The linker only sees the existing module dependency markers and
ordinary bytecode. No importer may optimize a dependency body from its public
interface.

`cdbc-cache 0.2` schema-3 records must distinguish products by at least:

```text
source hash
public interface hash
dependency interface hashes
optimization level
optimizer pipeline fingerprint
```

The fingerprint must be deterministic and versioned. A missing or mismatched
optimization identity follows the existing source-fallback or strict-rejection
policy; it must never reuse a product compiled under another optimizer.
Schema-2 manifests are stale and follow the existing cold-cache repair path.
The default identity is `optimization_level = "O0"` and
`optimizer_pipeline = "m7-ssa-o0-v1"`; explicit O1 products use
`optimization_level = "O1"` and
`optimizer_pipeline = "m7-ssa-o1-copy-phi-const-branch-dce-reach-thread-v6"`. A changed identity
rebuilds only the affected product and does not imply a public-interface
change.

## Proposed files and responsibilities

| File | Responsibility |
| --- | --- |
| `include/ControlFlowGraph.hpp`, `src/ControlFlowGraph.cpp` | block formation, successors/predecessors, CFG verifier, offset map |
| `include/Dominance.hpp`, `src/Dominance.cpp` | reachable dominators, immediate-dominator tree, dominance frontiers, verifier |
| `include/BindingMetadata.hpp` | snapshot-local binding identity and conservative storage contract |
| `include/SSA.hpp`, `src/SSA.cpp` | SSA values, memory slots, phi insertion, rename, verify, de-SSA |
| `include/Optimizer.hpp`, `src/Optimizer.cpp` | options, pass ordering, effect classification, counters, fingerprint |
| `include/IR.hpp`, `src/IR.cpp` | carry explicit binding metadata and preserve remappable internal IR data |
| `src/IRCompiler.cpp` | emit binding/storage facts at the lowering boundary |
| `src/main.cpp` | parse optimization/debug dump options and pass config consistently |
| `include/ModuleCache.hpp`, `src/ModuleCache.cpp`, `src/main.cpp` | include optimizer identity in product reuse decisions |
| `tests/` | unit, golden, artifact, module-cache, parity, and malformed coverage |

`BytecodeCompiler`, `BytecodeTextEmitter`, `vm-rs/src/format.rs`, and
`vm-rs/src/vm.rs` need no semantic changes for the first internal pipeline
slice. They are updated only if a later optimization decision changes a
public artifact or runtime contract.

## Verification matrix

The first implementation should add focused cases without changing existing
expected output:

| Area | Required proof |
| --- | --- |
| CFG | diamonds, loops, nested loops, fallthrough, malformed targets |
| SSA | dominance, phi incoming order, loop backedges, critical-edge copies, verifier failures |
| O0 | linear IR/CFG/SSA/de-SSA semantic round trip and unchanged goldens |
| O1 | constant/copy/branch simplification with exact output and exit behavior |
| effects | prints, assignments, closures, callbacks, aggregate mutation, traps, evaluation order |
| backend | C++ bytecode text, Rust parser/linker/VM, runtime-error parity |
| modules | imports, re-exports, dependency anchors, per-module optimization, cache invalidation |
| debug | source ranges remain valid; O0 trace remains deterministic |
| safety | malformed optimizer input, sanitizer build, `git diff --check` |

The canonical repository verification suite remains the release gate. New
optimizer tests should be independently named in the inventory rather than
hidden inside an existing golden count.

## Risks and mitigations

- Closure mutation may be misclassified as a local: require explicit capture
  metadata and default unknown storage to memory.
- A pass may move a runtime trap: classify trap-capable operations and add
  negative/evaluation-order fixtures before enabling motion.
- Deleted instructions may invalidate dependency offsets or debug ranges: use
  one remapping service and validate every emitted offset against the final
  program.
- Optimized products may be served from an old cache: include the optimizer
  fingerprint in the cache identity and test stale/missing records.
- SSA may increase register count: defer physical allocation until liveness and
  coalescing are measured; preserve a correctness-first virtual register path.
