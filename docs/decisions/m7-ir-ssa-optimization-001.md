# M7-IR-SSA-001: internal SSA and optimization boundary

Status: proposed overall on `feat/ssa-optimization-design`. The CFG
foundation, SSA structural shell, deterministic dominance-analysis,
phi-placement, binding/effect-contract, SSA memory-slot rename, de-SSA
copy-plan, internal linear-layout, ordinary-IR adapter, verified program-level
adapter/rebuild boundary, conservative internal O1 value-simplification, O0/O1
optimization/cache identity, explicit O1 CLI/module-product integration,
proven-safe primitive constant folding, block-preserving known-condition branch
normalization, post-de-SSA unreachable-block pruning, and redundant
fallthrough-jump removal, and threading through empty jump-only blocks are
implemented on this branch. Default O0 remains the compatibility path; general
block merging/jump threading, physical register allocation, and broader
optimization remain unadmitted.

## Question

How should the compiler introduce control-flow analysis, SSA values, and
optimization while preserving the source language, closure cell semantics,
the existing bytecode contract, and C++/Rust VM parity?

## Decision

Add an internal control-flow/SSA pipeline between the existing linear register
IR and `BytecodeCompiler`:

```text
AST + DeclarationIndex
  -> existing linear register IR
  -> CFG construction
  -> SSA construction and verification
  -> selected optimization passes
  -> de-SSA and virtual-register IR
  -> existing bytecode compiler
  -> cdbc 0.1 / Rust VM
```

The front end continues to own language meaning and binding resolution. The
optimizer is a private compiler service; it does not re-type-check AST nodes,
introduce source syntax, or make the Rust VM interpret SSA.

The default optimization level is `0`. An explicit optimization level is
required before any optimized IR or bytecode is produced. This keeps existing
IR, bytecode, artifact, debugger, and golden output stable while the new
pipeline is validated.

## SSA and memory boundary

The current virtual registers already have single-definition behavior for most
temporary expression values, but the current IR has no explicit basic blocks
or merge values. The SSA layer therefore makes control-flow joins and mutable
binding state explicit rather than treating the current register numbering as
complete SSA.

The first SSA representation contains two kinds of state:

- `SSAValue` values for expression results and other single-definition
  temporaries;
- `MemorySlot` state for runtime bindings represented by `LoadVar`,
  `StoreVar`, and `AssignVar`.

Only a binding proven to be local, non-captured, non-address-taken, and
unobservable outside its function may be promoted from `MemorySlot` to SSA.
Top-level/module bindings, exported names, function bindings used to build
closures, captured cells, and bindings without explicit storage metadata stay
on the existing runtime-cell path. The optimizer must never infer this policy
from a name string or from a source-level scope guess.

Field and index operations remain memory effects. The first slice does not
introduce alias analysis or Memory SSA. Field/index writes conservatively
invalidate related value assumptions, and calls, native calls, callbacks, and
unknown member operations act as effect barriers. This preserves the current
closure, array, map, struct, and mutation semantics.

## CFG and SSA invariants

The CFG builder splits each main/function instruction stream at the entry,
every jump target, every instruction after a conditional/unconditional jump,
and every terminator. Each block has deterministic IDs, ordered predecessor
and successor lists, and exactly one terminator or an implicit fallthrough that
is made explicit during normalization.

The SSA builder uses dominance and dominance frontiers for promotable memory
slots. A phi node has one incoming value per predecessor and is keyed by the
predecessor block, so predecessor order cannot alter semantics. Critical edges
are split when de-SSA needs an edge-local copy.

The verifier rejects:

- uses whose definitions do not dominate the use (with the normal per-edge
  rule for phi operands);
- duplicate definitions or duplicate phi incoming edges;
- missing incoming values at a join;
- invalid block IDs, malformed terminators, or non-reachable references; and
- instructions whose operand/result shape does not match the opcode contract.

Runtime traps are not modeled as freely movable pure operations. Division,
dynamic type assertions, indexing, field access, calls, native calls, and
other operations that may fail remain ordered with respect to the source
evaluation sequence unless a later pass proves both safety and equivalence.

## Initial optimization contract

The first enabled level (`O1`) is deliberately small and local. Its intended
contract includes:

- unreachable-block removal and jump threading;
- block merging and redundant jump removal;
- copy propagation and phi simplification;
- constant propagation and constant folding only when the operation is known
  to succeed and has no observable side effect;
- conditional branch folding when the condition value is proven constant; and
- dead-code elimination for non-trapping, side-effect-free instructions whose
  results are unused.

The currently admitted control-flow step is narrower than full CFG folding:
after verified de-SSA lowering, a known primitive condition may change its
`JumpIfTrue`/`JumpIfFalse` into an ordinary `Jump`, after which a rebuilt CFG
identifies and removes only unreachable blocks/instructions. Retained
instruction order, source/insertion maps, and dependency-anchor offsets are
remapped deterministically. A later local cleanup removes only an unconditional
`Jump` whose target is the next retained instruction, and a threading step may
skip a block containing only another unconditional `Jump`. General block
merging and jump threading across non-empty blocks remain future work. Keeping
all transformations on verified ordinary IR preserves the critical-edge copy
contract established by de-SSA.

The initial passes do not perform inlining, loop-invariant code motion,
speculative devirtualization, vectorization, allocation sinking, or CSE across
calls, stores, callbacks, aggregate mutation, or potentially trapping reads.
Object construction and function creation remain conservatively retained until
their allocation/identity effects have an explicit contract.

Later scalar promotion may remove redundant loads/stores for eligible local
bindings. It is a separate stage because source tracing currently displays
runtime cells. C1A now defines the current O1 contract: runtime-cell locals
remain visible, while source locations and semantic trace events stay stable
even when optimized line events and instruction numbers change.

## De-SSA and bytecode compatibility

Phi nodes are lowered to parallel copies on predecessor edges. Critical-edge
splitting and a temporary-copy rule handle cycles. The result is ordinary
`IRInstruction` data using the existing `IROp` set; no phi instruction is
serialized and no new bytecode opcode is required.

The optimizer preserves an origin span for every retained instruction. A
folded instruction uses the source range of the expression it represents;
generated edge copies inherit the original IR span when a safe anchor exists,
including when O1 removed that source instruction before de-SSA. The C1A
contract is recorded in [`c1a-optimized-debug-001.md`](c1a-optimized-debug-001.md).
The existing `debug_sources`, `debug_locations`, and `debug_ranges`
emitter/linker behavior remains the artifact boundary.

Module dependency markers are treated as ordered CFG anchors, not ordinary
instructions. Any transformation that changes instruction offsets must return
an old-to-new offset map and update `IRModuleDependency::instructionOffset`.
Independent module products are optimized per module; no cross-module or
link-time optimization is introduced.

The module-cache key must include the optimization level and a stable
optimizer-pipeline fingerprint before optimized products can be reused. A
cache record produced for one pipeline must not be silently reused for another
pipeline. The `.cdbc 0.1` wire format itself remains unchanged in this design.

## Proposed compiler interfaces

The implementation should add private/internal services with narrow ownership:

```text
include/ControlFlowGraph.hpp  src/ControlFlowGraph.cpp
include/Dominance.hpp         src/Dominance.cpp
include/SSA.hpp               src/SSA.cpp
include/Optimizer.hpp         src/Optimizer.cpp
```

The exact names may change during implementation, but responsibilities should
remain separate:

- CFG construction and normalization consume one `IRFunction` or main stream;
- SSA construction, verification, and de-SSA operate on the CFG form;
- the pass manager owns optimization levels, pass ordering, statistics, and
  the pipeline fingerprint; and
- `IRCompiler` supplies explicit binding/storage metadata rather than exposing
  AST or `DeclarationIndex` internals to individual optimization passes.

`BytecodeCompiler` should remain a lowering boundary, not an optimizer. The
Rust VM should remain an execution boundary, not a second implementation of
the optimization pipeline.

## CLI and tooling boundary

The implementation phase may add:

```text
--opt-level 0|1|2
--dump-cfg
--dump-ssa
```

`--opt-level 0` is the default. `--dump-cfg` and `--dump-ssa` are developer
diagnostics and must not alter emitted artifacts. O2 is reserved until scalar
promotion, debug-local policy, and register-pressure measurements are
resolved; accepting O2 before that work would create a misleading contract.

Existing `--ir`, `--bytecode`, `--emit-bytecode`, and
`--emit-module-bytecode` remain valid. Their default output remains the O0
path. Optimized artifacts are expected to have the same runtime behavior, but
optimized trace event count and local visibility are not promised until a
debug-local location policy is implemented.

## Verification gate

Before the first implementation slice, establish a baseline from the current
`origin/master` checkout. Each later slice must prove:

- CFG/SSA unit tests for diamonds, loops, nested loops, unreachable blocks,
  critical edges, phi cycles, and malformed input;
- an O0 round-trip test showing that CFG/SSA/de-SSA preserves instruction
  semantics and source spans;
- an O0/O1 equivalence corpus covering branches, loops, closures, assignments,
  aggregate mutation, callbacks, runtime errors, and evaluation order;
- C++ emitted artifact versus Rust VM execution parity;
- imported modules, re-exports, independent products, dependency anchors, and
  cache invalidation for optimization configuration; and
- existing golden, CTest, canonical verification, artifact, debugger, Rust,
  and `git diff --check` gates.

The optimizer is not complete merely because an SSA dump looks correct. The
completion proof is semantic parity plus a verifier and cache/debugging
contract.

## Implemented CFG foundation sub-slice

This branch implements the first narrow foundation without changing compiler
semantics:

- `ControlFlowGraph` builds deterministic blocks for the main stream and each
  function stream, including a synthetic exit block;
- conditional edges are ordered as fallthrough then taken, while predecessor
  lists remain deterministic and deduplicated;
- unreachable blocks remain present and are marked for a later reachability
  pass;
- module dependency offsets are retained as block-local anchors, including the
  end-of-stream anchor; and
- the structural verifier checks ranges, edge symmetry, instruction mapping,
  reachability metadata, and dependency-anchor consistency.

The focused CTest case is `control_flow_graph`; it is registered in the
verification inventory as `ctest.control_flow_graph`. This foundation does
not construct SSA values, insert phi nodes, simplify IR, or alter bytecode.

The branch also implements the SSA structural shell in `include/SSA.hpp` and
`src/SSA.cpp`: value IDs, ordered phi incoming records, entry parameters,
conservative memory-slot storage classes, CFG/SSA block alignment, duplicate
definition and undefined-use checks, and phi predecessor completeness/order
validation. The shell remains independent of construction and optimization.

The branch also implements deterministic dominance analysis in
`include/Dominance.hpp` and `src/Dominance.cpp`: reachable-subgraph dominator
sets, immediate-dominator tree children, and dominance frontiers. Unreachable
blocks have no dominance metadata; reachable synthetic exit blocks participate
normally, and unreachable predecessor edges are excluded from frontier
calculation. This slice does not place phis or rename SSA values.

The branch also implements iterated dominance-frontier phi placement through
`placePromotableMemoryPhis`. It accepts block-level definition sites supplied
by a future IR lowering boundary, places phis only for `Local` slots, ignores
unreachable definitions, and never places values in the synthetic exit block.
The result is placement metadata ordered by slot and block; the placement API
itself remains metadata-only.

The admitted construction slice adds `renamePromotableMemorySlots` in
`include/SSA.hpp` and `src/SSA.cpp`. It allocates deterministic values for
parameters, expression results, and local-slot phis; walks the verified
dominator tree; eliminates Local `LoadVar`/`StoreVar`/`AssignVar`; fills phi
incoming values in CFG predecessor order; seeds Local slots from parameter
values; and preserves non-promotable memory operations. It rejects invalid or
undefined local slots, malformed variable instructions, duplicate raw virtual
register definitions, and non-dominating raw uses. Unreachable blocks are
retained through a deterministic linear rename but do not participate in phi
placement. This service is internal and is not connected to the default
compiler or bytecode path. The SSA verifier additionally checks reachable-use
dominance, same-block definition order, phi-edge availability, and basic
opcode operand shape.

The branch also implements `planSSADeSSACopies`, which produces deterministic
edge-local sequential move bundles for phi lowering, removes identity moves,
allocates one fresh temporary per parallel-copy cycle, and marks critical
edges. It intentionally does not split CFG edges, remap instruction offsets,
or connect to the bytecode path.

The branch also implements `lowerSSADeSSACopies`, which materializes that plan
as an internal linear layout. It places non-critical copies at a unique
successor entry or predecessor exit, inserts critical fallthrough split blocks
in place, appends critical branch-target split blocks deterministically, and
rewrites block-entry branch targets. The result carries source instruction and
insertion-boundary maps plus remapped module dependency offsets. It remains an
SSA-owned internal result: it does not convert to `IRFunction`, alter the
original CFG, or connect to bytecode, debug-local, or cache identity paths.

`lowerSSADeSSAToIR` provides the internal lowering boundary. It reuses the
existing `IROp` and operand fields, preserves SSA value IDs as virtual-register
indices, computes the required register count, forwards caller-supplied
parameter names and binding metadata, and carries source/offset maps plus
synthetic-copy provenance. The explicit O1 program path invokes the verified
program-level adapter around this boundary; O0 continues to use the original
linear stream directly.

The branch also adds `optimizeSSA` in `include/Optimizer.hpp` and
`src/Optimizer.cpp`. O0 is a verified identity result. The current internal O1
slice propagates `Copy` values, removes trivial phis only when their replacement
dominates the join, and removes unused value-producing instructions only when
`irEffectSummary` classifies them as pure. The O1 constant pass uses the
constant evaluator only for finite serializable primitive results whose
operation is proven to succeed; it materializes new values through the
program-level constant pool. Potential traps, memory operations, allocation,
calls, and output remain unchanged; CFG pruning only removes blocks proven
unreachable after known-condition normalization. The explicit O1
CLI and module-product paths call `optimizeIRProgram`, which applies this
verified boundary to the main stream and every nested function before existing
bytecode lowering. After de-SSA, O1 normalizes only known primitive conditions
on the verified ordinary IR: `JumpIfTrue`/`JumpIfFalse` become ordinary
`Jump` instructions, removes only unreachable ordinary-IR blocks, and removes
redundant fallthrough jumps while remapping source/insertion and dependency
offsets. A jump may thread only through a jump-only block; this placement
preserves critical-edge copy validation. General block merging, CFG rewriting,
and threading across non-empty blocks remain future work.
`ctest.optimizer` and `optimizer_cli` cover
the O0 identity, copy/phi
behavior, pure dead-code deletion, proven-safe constant folding,
known-condition branch normalization, reachability/jump cleanup, jump-only
threading, trap retention,
malformed-input
rejection, C++/Rust output parity, and O0/O1 module-cache identity.

The constant-evaluation boundary is now explicit in
`evaluateSSAConstantUnary` and `evaluateSSAConstantBinary`. A result is
`Folded` only for finite, cdbc-serializable primitive values whose runtime
operation is known to succeed. Division by zero and statically known operand
type failures are classified as `RuntimeTrap` and remain eligible for runtime
execution rather than becoming compile-time diagnostics. Non-finite inputs or
results are `NonFinite` because the current artifact constant contract rejects
them; non-primitive values and unsupported opcodes are `Unsupported`. This
classifies legality for the O1 constant pass. Folded operations remain ordinary
`Constant` instructions with existing IR/bytecode opcodes; runtime traps and
non-finite results remain unmodified.

The internal `optimizeIRFunction` adapter freezes the one-stream invocation boundary:
one ordinary `IRFunction` and its dependency anchors are lifted as one stream,
checked through the selected SSA optimizer, and lowered back with source spans,
original/insertion offset maps, and remapped dependency offsets. Function
parameters remain runtime-cell bindings during this conservative lift, so the
adapter restores the ordinary parameter list after SSA lowering. O0 preserves
at least the input virtual-register count; O1 reuses virtual SSA IDs and does
not perform physical register allocation or coalescing. This adapter remains
an internal service; the program-level wrapper is what the explicit O1 CLI and
module-product paths invoke.

`optimizeIRProgram` now applies that adapter to the anonymous main stream and
every function-table entry in the existing index order. Its
`SSADeSSAProgramResult` retains one verified offset-map result per stream,
preserves function names, parameters, binding visibility, and every
`MakeFunction` index sequence, and remaps only main-stream dependency offsets.
`verify` checks those invariants against the source `IRProgram`; `rebuild`
copies the source constant pool, interns any newly folded primitive values,
copies the name table, sources, and canonical bindings, and replaces only the
verified streams. This remains an opt-in boundary: it
does not invoke `IRCompiler` itself, and nested function results cannot carry
module dependency anchors. The explicit O1 caller rebuilds the original
`IRProgram` tables around the verified result and then reuses the existing
bytecode/artifact emitters.

The selected debug-local policy keeps source-visible runtime-cell operations in
the default O1 contract; `renamePromotableMemorySlots` remains an internal
experiment until optimized local materialization and trace mapping are
specified. The module-product cache records `optimization_level` and
`optimizer_pipeline` in schema 3 and includes both in the length-delimited
cache key. The default identity is `O0` / `m7-ssa-o0-v1`; explicit O1 products
use `O1` / `m7-ssa-o1-copy-phi-const-branch-dce-reach-thread-merge-v7`. Schema 2 manifests are stale and take
the existing cold-cache repair path. This does not change the `cdbc 0.1`
artifact or Rust VM wire format.

The O1 boundary now also admits a conservative post-de-SSA non-empty block
merge. A block is moved next to its unique successor only when the predecessor
has that successor as its sole edge, the successor has no other predecessor,
all fallthrough edges remain valid in the new deterministic order, the
`MakeFunction` reference sequence is unchanged, and module dependency offsets
remain ordered after remapping. The merge removes only the predecessor's
unconditional jump; general CFG rewriting and arbitrary non-empty block
threading remain deferred. The implementation explicitly validates implicit
fallthrough edges for conditional terminators and rejects self-loop candidates
before attempting layout reordering.

The branch now freezes the internal binding/effect contract in
`include/BindingMetadata.hpp` and `include/IR.hpp`. `IRBinding` carries a
snapshot-local `BindingId`, resolved name, and explicit storage class; only
`Local` is promotable, while unknown or absent metadata remains conservative.
Variable memory instructions may carry the binding ID without changing their
printed IR or bytecode representation. `irEffectSummary` conservatively
classifies memory reads/writes, traps, allocation, calls, observability, and
control flow. `IRCompiler` now populates this metadata for source declarations,
function parameters, captures, module/exported bindings, and synthetic cells;
the canonical binding table is program-global and function tables contain only
per-body visibility references. Interface-only imported values without a
snapshot binding ID remain conservative.

## Non-goals

This proposal does not add language syntax, change type inference or nullable
flow, change closure ownership, add a new runtime representation, change
`cdbc 0.1`, optimize across module boundaries, add a JIT, add garbage
collection, or make optimization mandatory.

## Deferred decisions before making optimized lowering the default

The branch decision is to keep the current unique-predecessor/unique-successor
merge as the complete O1 CFG rewrite boundary. General non-empty block merging
and threading are deferred until a layout-aware edge-rewrite contract can prove
critical-edge handling, dependency-anchor remapping, source/debug mapping, and
semantic parity. The current O0 default and `cdbc 0.1` boundary do not require
that broader rewrite.

The following remain before making optimized lowering the default:

1. the register-allocation strategy and its source-location mapping;
2. the optimized trace-local materialization contract and whether it is strong
   enough to replace the O0 default.
