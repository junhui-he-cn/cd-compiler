# M7-IR-SSA-001: internal SSA and optimization boundary

Status: proposed overall on `feat/ssa-optimization-design`. The CFG
foundation, SSA structural shell, deterministic dominance-analysis,
phi-placement, binding/effect-contract, and SSA memory-slot rename sub-slices
are implemented on this branch; de-SSA and optimization remain unadmitted.

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

The first enabled level (`O1`) is deliberately small and local:

- unreachable-block removal and jump threading;
- block merging and redundant jump removal;
- copy propagation and phi simplification;
- constant propagation and constant folding only when the operation is known
  to succeed and has no observable side effect;
- conditional branch folding when the condition value is proven constant; and
- dead-code elimination for non-trapping, side-effect-free instructions whose
  results are unused.

The initial passes do not perform inlining, loop-invariant code motion,
speculative devirtualization, vectorization, allocation sinking, or CSE across
calls, stores, callbacks, aggregate mutation, or potentially trapping reads.
Object construction and function creation remain conservatively retained until
their allocation/identity effects have an explicit contract.

Later scalar promotion may remove redundant loads/stores for eligible local
bindings. It is a separate stage because source tracing currently displays
runtime cells, and optimized artifacts must either retain source-visible local
materialization or explicitly use a debug-optimization contract.

## De-SSA and bytecode compatibility

Phi nodes are lowered to parallel copies on predecessor edges. Critical-edge
splitting and a temporary-copy rule handle cycles. The result is ordinary
`IRInstruction` data using the existing `IROp` set; no phi instruction is
serialized and no new bytecode opcode is required.

The optimizer preserves an origin span for every retained instruction. A
folded instruction uses the source range of the expression it represents;
generated edge copies have no user source location unless a safe existing span
can be carried. The existing `debug_sources`, `debug_locations`, and
`debug_ranges` emitter/linker behavior remains the artifact boundary.

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

## Open decisions before de-SSA and optimization

The following must be resolved in the first implementation decision revision:

1. how de-SSA handles critical-edge splitting, parallel-copy cycles, and
   instruction-offset remapping;
2. whether O1 promotes non-captured locals or only optimizes explicit SSA
   temporaries;
3. the source-level local policy for `compiler-design-vm trace` on optimized
   artifacts;
4. the stable optimizer fingerprint and its `cdbc-cache 0.2` representation;
5. the precise constant-evaluation error/trap classification; and
6. the register-allocation strategy and its source-location mapping.
