# Compiler Design Roadmap

This is the active roadmap for the compiler and language. It was reset on
2026-07-27 against `master` at `e90a752` (the M5D trace merge). Completed
implementation details are intentionally not kept in the active queue. Their
contracts remain in `README.md`, `docs/language-grammar.ebnf`, the focused
decision records, tests, and Git history.

The current release line is `0.1`, with canonical version `0.1.0` and immutable
tag `v0.1.0`. See `docs/versioning.md` for branch and release rules.

## How to use this document

Every new slice must be independently reviewable and contain:

1. a concrete deliverable and an explicit non-goal boundary;
2. a migration path that reuses the production compiler, artifact, and VM
   services;
3. a quantitative gate bound to named inventory case IDs and commands; and
4. a condition for deleting any compatibility or duplicate path.

An item listed under the completed baseline is not an active task. A planned
item becomes active only after its decision record is written and its focused
corpus is identified. New syntax, semantic behavior, artifact changes, or VM
behavior must update the implementation contract and tests together.

## Current master baseline

| Area | `master` status | Evidence / remaining boundary |
| --- | --- | --- |
| Verification | M0A-M0D complete | `tests/verification_inventory.json` is revision `m0d-2026-07-22-r1` with 1,868 checks; `tests/run_verification.py` is the canonical runner. |
| Semantic front end | M1A1-M1F complete | `SourceIdentity`, `LosslessSourceView`, `DeclarationIndex`, shared type/pattern metadata, and HIR-only IR lowering are shipped. |
| Language semantics | Admitted M2A flow slices and M2B recovery slices complete | M2A `FLOW-001..021`, M2B type recovery, parser recovery `001..003`, and lexer recovery `001` are shipped; the broader flow policy remains open below. |
| Generic capabilities | M6-LANG-001 and M6-LANG-HASH-001 complete | Compile-time `Eq`/`Ord`/`Hash` bounds, canonical `+` conjunctions, inference, explicit arguments, comparator/hash forwarding, public interfaces, deterministic hash native execution, and module-cache validation are shipped; runtime capability dispatch remains outside the contract. |
| Modules and cache | M3A graph/interface slices plus M3B artifact/cache boundaries complete | Independent module products, linker inputs, `.cdi` interfaces, `cdbc-cache 0.2`, invalidation, and safe source fallback are shipped. Removing fallback is not approved. |
| Artifact/runtime | M4A validation and M4B debug metadata complete | `cdbc 0.1` remains the contract; validation, module identity, source ranges, and link-time debug rebasing are shipped. |
| Formatter | M5A-FORMAT-001..008 complete | `--format`, `--format-check`, lossless comments/trivia, idempotence, blank-line/trailing-comma policies, bounded list wrapping, and invalid-input rejection are shipped. |
| Language server | M5B-LSP-001..017 complete | Single-document and opened-virtual-workspace queries are shipped; closed imports and unknown/dynamic receivers remain outside the current boundary. |
| REPL | Not shipped on `master` | A partial prototype exists only on `feat/m5c-repl`; it is not part of the current baseline. |
| Source debugger | M5D-DEBUG-001 complete | One-shot `compiler-design-vm trace` with source events, stacks, locals, output, returns, and failures is shipped; interactive control is explicitly deferred. |

The detailed records for completed slices are deliberately left in
`docs/decisions/` as evidence, but they are no longer repeated as roadmap
work. The completed groups are:

- M0A-M0D, M0.5A, and M0.5B;
- M1A1, M1A2, M1B, M1C, M1D, M1E1, M1E2, M1E3, and M1F;
- the admitted M2A flow and M2B recovery slices;
- M3A graph/interface work, M3B artifact/cache/boundary work, and M4A/M4B;
- M5A-FORMAT-001..008, M5B-LSP-001..017, M5D-DEBUG-001, M6-LANG-001,
  M6-LANG-HASH-001, and M6-LANG-OPERATOR-001A..001C.

These names are a completion record, not a to-do list.

## Active constraints and open contracts

The following behavior is intentionally retained while new specifications are
being designed:

- Direct single-file and ordered direct-multi-file CLI inputs remain one entry
  program. Import-aware graph compilation is not allowed to silently replace
  that established path.
- Valid interface/product cache hits may skip dependency-body checking and
  lowering. Cold builds and repairs still use source fallback. A future slice
  may tighten creation/repair policy only after an explicit decision and
  compatibility corpus.
- The current nullable-flow contract is conservative for unresolved dynamic
  indexes, aliases, broader `for-in` exits, and other unsupported mutation
  paths. A new rule requires a soundness decision plus positive, negative, and
  invalidation fixtures.
- The LSP virtual workspace currently reasons about synchronized/opened
  modules. Closed or disk-only imports and unknown/dynamic receiver completion
  are separate work, not implicit bug fixes.
- `cdbc 0.1` and its metadata-free compatibility behavior remain stable. A
  debugger or tool slice must reuse existing debug metadata before proposing a
  new artifact section or version.
- M5C is parked until its branch-only prototype is re-audited against the
  then-current `master`; branch code does not count as shipped behavior.

## Latest admitted slice

### M6-LANG-HASH-001: static hash capability and deterministic hash entry point

**Status:** complete for the language-support boundary; hash-based containers,
generic map-key admission, mutable-key ownership, and user-defined capability
implementations remain deferred.

**Purpose:** unblock generic library APIs that need an explicit hash contract
without adding concrete `HashMap`/`HashSet` implementations to the compiler
repository.

**Deliverable:** accept `T: Hash` and canonical capability conjunctions such as
`T: Eq + Hash`; type-check `hash(value)` for concrete and constrained generic
values; preserve the constraints through public interfaces and `.cdi` sidecars;
and execute a deterministic typed 32-bit FNV-1a native call in the Rust VM.

**Boundary:** keep `cdbc 0.1` unchanged apart from the registered native name;
do not add a runtime capability dictionary, generic map-key admission, or
mutable-key ownership rule. Reference values use identity hashing and enum
variants use structural payload hashing under the shared C++/Rust contract.

**Quantitative gate:** cover local, imported, namespace/re-exported, inferred,
and `Eq + Hash` calls; unconstrained diagnostics; interface text; C++ value hash
constants; Rust VM unit tests; and emitted artifact execution. Run the focused
golden/artifact/Rust checks, CTest, canonical verification, and `git diff --check`.

### M6-LANG-HASH-002: stable generic hash keys (feature branch)

**Status:** implemented on `feat/issue-15-stable-hash-keys`; not yet part of
`master` until its branch is integrated.

**Decision:** generic `HashSet<T: Eq + Hash>` and
`HashMap<K: Eq + Hash, V>` use the existing equality/hash law and identity-
stable semantics for mutable arrays, maps, functions, and named structs.
Aliases may mutate stored keys without invalidating membership. The public
library uses array-backed buckets and preserves insertion-order snapshots; the
built-in `map<K,V>` remains primitive-keyed.

**Evidence:** `docs/decisions/m6-language-hash-002.{md,json}`,
`library/hash_collections.cd`, the isolated library fixture
`data_structures_hash_collections`, C++/Rust reference-hash tests, and the
unconstrained generic diagnostic fixture.

### M6-LANG-OPERATOR-001A: builtin string ordering

**Status:** complete in the current working tree under the resolved
`M6-LANG-OPERATOR-001` design; user-defined struct declarations remain a later
slice.

**Purpose:** extend the existing four ordering operators to `string` using
Unicode scalar-value lexicographic semantics. Preserve numeric ordering,
compile-time-only generic capabilities, the current bytecode instruction set,
and the `.cdbc 0.1` artifact boundary.

**Deliverable:** accept `<`, `<=`, `>`, and `>=` for known string operands;
execute the same behavior in the Rust VM; add an artifact/runtime parity
fixture; and update the public language documentation. Do not add the
`operator` declaration syntax or user-defined dispatch in this slice.

**Migration:** reuse the existing `Ord` capability and comparison IR/bytecode
operations. Extend only their builtin string type checks and runtime operands;
do not add an opcode, runtime operator table, hidden generic comparator, or
change to `cdbc 0.1`.

**Quantitative gate:** `rust_vm.artifact.string_ordering.emit` and
`rust_vm.artifact.string_ordering.run` cover all four symbols, ASCII ordering,
non-ASCII scalar ordering, prefix ordering, equality boundaries, and
canonically equivalent but differently encoded sequences. Run the focused
golden/artifact/Rust checks, refresh the verification inventory, and pass
`git diff --check`.

**Delete the old path when:** none; numeric comparison remains the compatibility
path. This slice is complete only when C++ emission and Rust execution agree,
and the later user-defined operator slice can reuse the same comparison
operations without introducing a second builtin path.

### M6-LANG-OPERATOR-001B: local named-struct comparison operators

**Status:** complete for local named structs in the defining compilation unit
and linked `.cdbc 0.1` emission. Public interface/cache propagation remains a
later slice.

**Purpose:** add `operator <`, `operator <=`, `operator >`, and `operator >=`
inside `impl` blocks. The left operand is the implicit `this` receiver, the
right operand is one same nominal struct parameter, and the result is `bool`.
Dispatch is resolved by the left operand and lowered through the existing
ordinary function-call path.

**Deliverable:** reserve `operator`, parse and print all four declarations,
validate receiver/parameter/return/duplicate rules, reject foreign module
implementations, and cover local generic receivers plus C++/Rust linked-artifact
parity. Numeric and string comparisons retain their existing builtin paths.

**Boundary:** user-defined operators are not exported through `.cdi` or module
cache products in this slice, independent module products and imported/re-exported
operator dispatch remain deferred, and custom structs do not satisfy `T: Ord`.
`==`, `!=`, arithmetic, unary, logical, compound-assignment, enum, primitive,
and dynamic operator behavior remain deferred.

**Quantitative gate:** cover all four local symbols, invalid symbol recovery,
missing implementation, wrong right type, wrong return, wrong parameter, and
duplicate implementation diagnostics; run focused golden, Rust VM, formatter,
CTest, and canonical verification; refresh the verification inventory and pass
`git diff --check`.

## Operator slice status

The public operator metadata/interface phase, source-backed imported dispatch,
and independent module-product/linker parity are complete. The operator
contract remains ordinary function-call lowering with no new opcode or runtime
operator table.

### M7-LANG-OPTIONAL-001: canonical optional type syntax

**Status:** implementation complete; `optional<T>` is the only nullable type
spelling accepted by the language.

**Purpose:** make `optional<T>` the readable, compositional spelling for a
value that is either `T` or `nil`, with the existing nullable-flow semantics.

**Deliverable:** parse nested `optional<T>` annotations anywhere a type
annotation is accepted; preserve the spelling in AST output; resolve it to the
existing `StaticType::Nullable`; expose its inner type through LSP type
navigation; and migrate the public README, grammar, decision record, and
focused success/parse/type-error fixtures. The C++ IR, bytecode, `.cdbc 0.1`,
and Rust VM representations remain unchanged. Optional function values such as
`optional<fun(number): number>` are supported, as are nullable parameter and
return types written with `optional<...>`.

**Source rule:** recognize `optional` contextually only when followed by `<`, so
existing identifiers named `optional` remain valid. Postfix `T?` is rejected
with a parser diagnostic directing users to `optional<T>`. New documentation
and fixtures use `optional<T>`, including array elements, nullable arrays,
struct fields, and function parameter/return annotations.

**Quantitative gate:** the focused corpus must register and pass
`golden.success.optional_type_syntax.*`,
`golden.success.optional_function_values.*`,
`golden.parse_errors.optional_missing_argument`,
`golden.parse_errors.nullable_double_question`, and
`rust_vm.golden.optional_type_syntax.*`; then run the canonical inventory,
golden, CTest, boundary, malformed, artifact, LSP, debugger, Rust VM, Cargo,
and `git diff --check` commands from the verification contract.

## Planned follow-up specifications

These are deferred candidates after M6-LANG-001. They are not permission to
start implementation before their decision records and focused inventories are
written. In particular, the data-structure roadmap does not authorize adding
concrete data-structure code to the compiler slice.

### M6-LANG-OPERATOR-001C: public operator metadata and module products

**Status:** complete. Local implementation, public interface/cache shape,
source-backed imported dispatch, and independent module-product/linker parity
are covered by the resolved declaration, focused module-artifact regression,
and C++/Rust execution paths.

**Purpose:** extend the local operator implementation with public interface and
module-cache propagation, imported/re-exported dispatch, and independent module
product parity.

**Boundary:** keep `==`, `!=`, arithmetic, unary, logical, compound-assignment,
enum, primitive, dynamic, and generic capability-dictionary behavior deferred.
Generic algorithms continue to use explicit comparator values for custom
structs.

**Completed first phase:** exported and re-exported struct operators are
represented in canonical module-interface text, strict `cdi 0.1` sidecars,
public-interface hashes, and module-interface validation. Sidecars missing the
operator section follow the existing malformed/stale fallback or strict
rejection path.

**Completed imported-dispatch phase:** direct imports, namespace aliases,
re-exported structs, and generic receiver operators restore operator metadata
from the public interface and lower through the existing ordinary call path.
Imported operators retain resolved linkage names without requiring a local AST
method declaration; the source-backed C++ and Rust VM paths are covered by
focused fixtures and declaration-index validation.

**Completed gate:** independent `.cdbc 0.1` products preserve the owner
operator functions, importer dependency markers, and resolved linkage names;
Rust canonical dump, link, and execution agree with the C++ source-backed
operator goldens.

### M5D-DEBUG-002: Interactive breakpoints and stepping

**Status:** explicitly deferred. It remains a valid independent direction, but
interactive debugger work is postponed after M6-LANG-001 and is not currently
active.

**Purpose:** turn the shipped one-shot trace event boundary into a small,
deterministic interactive debugger session without changing the language or
the `cdbc 0.1` artifact.

**Deliverable:** add a Rust VM debugger entry point and line-oriented session
protocol for source breakpoints, `continue`, `step`, `next`, and `quit` (the
exact command spelling and event schema must be fixed in
`docs/decisions/m5d-debug-002.{md,json}` before implementation). A pause event
must identify the module/source/range, active stack, current frame, and
available locals. Nested calls, loops, imported modules, returns, and runtime
failures must have deterministic stop order.

**Migration:** build on `M5D-DEBUG-001`, the VM instruction loop, and
`debug_sources`/`debug_locations`/`debug_ranges`. Keep `trace` output and
ordinary `run` behavior unchanged. Use explicit source-range matching and
retain the metadata-free unknown-location behavior. Do not add source
evaluation, register mutation, watch expressions, or a new source mapper in
this slice.

**Quantitative gate:** extend `tests/debugger_tests.py` with repeated-session
determinism, breakpoint hits, step/next ordering, nested/imported frames, and
runtime-failure coverage; register each independently measured case in the
inventory; pass the debugger CTest, Rust VM tests, and `git diff --check`.

**Delete the old path when:** all interactive events consume the shared M4B
metadata and VM event boundary, no debugger path guesses source locations, and
the old one-shot trace behavior remains covered as a compatibility case.

**Dependency:** current `master`, M4B debug metadata, and M5D-DEBUG-001.

### M5B-LSP-018: Closed-module workspace navigation

**Purpose:** extend the existing virtual workspace from opened documents to an
explicit workspace-root/import set, where open-document text overrides disk
text and closed imported modules are loaded through `FrontendSession` and the
module graph. Start with definition and references; keep completion, rename,
and cache persistence as separate boundaries.

**Required decision:** define workspace roots, import search order, disk/source
version precedence, diagnostics for unavailable modules, and whether an
unopened dependency may contribute a public declaration. Do not duplicate
name resolution in the LSP adapter.

**Gate:** multi-file protocol cases prove stable URIs/ranges, open-over-disk
precedence, direct imports, namespace aliases, re-exports, missing modules,
and no reads outside the declared workspace. CLI diagnostics and LSP ranges
must remain equivalent for the same source snapshot.

### M2A-FLOW-022: Advanced nullable-flow policy

**Purpose:** decide one sound extension for the currently conservative cases:
dynamic index targets, alias-specific field/index facts, or a normal `for-in`
exit. The first slice is a decision and corpus slice, not an implementation
rewrite.

**Required decision:** state the alias/mutation model, loop fixpoint or exit
proof, invalidation rules, diagnostic compatibility, and the exact cases that
remain conservative. A later implementation slice may accept only the proven
subset.

**Gate:** add positive, negative, mutation, alias, nested-loop, closure, and
cross-backend fixtures to the analysis inventory; preserve C++/Rust parity and
record zero unsound accepted programs in the bounded corpus.

### M3B-BOUNDARY-002: Strict module-cache creation and repair

**Purpose:** decide whether and when source fallback may be removed for missing
or invalid module products/manifests. This is a product-policy decision, not a
cleanup of the existing safe fallback.

**Required decision:** specify cold-build behavior, repair behavior, offline
behavior, strict versus compatibility modes, user diagnostics, and migration
for existing `cdbc-cache 0.2` records. The linked entry path and independent
module-product path must be considered separately.

**Gate:** change matrices cover missing, malformed, source-changed, public-
interface-changed, dependency-hash-changed, and stale-product cases, with
rebuild reports, diagnostics, linked artifacts, and VM execution asserted.

### M5C-REPL-001: Re-audit the incremental session prototype

**Status:** parked, not active. Rebase `feat/m5c-repl` onto the latest
`master`, inspect its JSON Lines/session/import behavior, and either split a
small compatible slice or discard the branch-only assumptions. No REPL claim
belongs in the baseline until that re-audit is merged and verified.

### M7-IR-SSA-001: Internal SSA and optimization pipeline

**Status:** the CFG foundation, SSA structural shell, deterministic dominance
analysis, phi placement, binding/effect contract, IRCompiler binding metadata
integration, and SSA memory-slot renaming are implemented on `master` through
the merged `feat/ssa-optimization-design` slice; de-SSA copy planning, an internal linear
de-SSA layout, an ordinary-IR adapter, a conservative internal O1
value-simplification service, the verified program-level main/function-table
adapter and rebuild boundary, and explicit `--opt-level 0|1` CLI/module-product
integration are also implemented on this branch. Default O0 lowering remains
the compatibility path, and proven-safe primitive constant folding,
block-preserving known-condition branch normalization, and post-de-SSA
unreachable-block pruning are also admitted on this branch. Block
merging/general CFG rewriting, physical register allocation, and broader
optimization are still proposed and are not shipped on `master`. The
design
and machine-readable decision are in
`docs/superpowers/specs/2026-07-30-ssa-optimization-design.md` and
`docs/decisions/m7-ir-ssa-optimization-001.{md,json}`.

**Purpose:** introduce explicit basic blocks, dominance/phi-based SSA, and a
small opt-in O1 pass pipeline behind the existing linear register IR while
preserving source semantics, closure cells, module-product boundaries,
`cdbc 0.1`, and C++/Rust execution parity.

**Initial boundary:** O0 remains the default. O1 may simplify CFGs, propagate
copies/constants, fold proven-safe primitive operations, simplify phis, and
remove only non-trapping pure dead code. Captured/module/unknown bindings,
aggregate aliasing, calls, callbacks, and runtime traps remain conservative.
SSA is de-lowered to the existing IR; phi nodes and optimizer metadata are not
serialized.

The branch has already frozen the conservative debug-local policy,
constant/trap classification, and `cdbc-cache 0.2` schema-3 optimization
identity. Explicit O1 program/module lowering now consumes those contracts;
the default O0 path remains unchanged and optimized trace-local equivalence is
not promised.
The current branch establishes CFG shape, SSA value/phi structure, dominance
analysis, phi placement, local-slot value allocation and renaming, edge-copy
planning, linear layout with critical-edge splits, branch/dependency remapping,
ordinary-IR adaptation, source binding/storage metadata, the conservative
effect table, internal copy/phi simplification, pure dead-code removal, the
constant-evaluation trap/non-finite boundary, proven-safe primitive constant
folding, post-de-SSA known-condition branch normalization and unreachable-block
pruning, O0/O1 cache identity,
conservative source-local trace policy, the single-stream and program-level
ordinary-IR optimizer adapters, explicit CLI/module-product O1 selection, and
verification.

**Required decision before default optimized lowering:** decide when the
compiler should make O1 the default and whether optimized trace-local
materialization is strong enough for that change. The explicit O1 boundary now
invokes the verified program-level result/rebuild adapter for the main stream
and nested functions in stable function-table order, preserves module anchors
and source mappings, and propagates the optimization identity to module-cache
products. The branch intentionally keeps O0 as the default, preserves virtual
register IDs, and defers physical allocation/coalescing to O2. The current O1
constant slice folds only finite serializable primitive expressions and
materializes their results through the existing constant pool; its branch
normalization converts known conditional jumps to ordinary jumps, then removes
only unreachable ordinary-IR blocks while remapping source and dependency
offsets. It does not merge blocks, preserving critical-edge copy validity.

**Gate:** CFG/SSA verifier and O0 round-trip tests; O0/O1 semantic parity over
control flow, closures, mutation, callbacks, traps, and evaluation order;
explicit CLI and independent module-product selection; C++/Rust artifact
parity; imported/re-exported module and cache coverage; and the canonical
verification suite.

## Dependency order

```text
master + shipped generic functions, callbacks, and module interfaces
  -> completed M6-LANG-001 static generic capability constraints
  -> completed M6-LANG-OPERATOR-001A builtin string ordering
  -> completed M6-LANG-OPERATOR-001B local struct comparison operators
  -> completed M6-LANG-OPERATOR-001C public operator metadata/cache shape and
     source-backed imported dispatch and independent module products/linker parity
  -> M7-LANG-OPTIONAL-001 canonical optional type syntax
  -> language-enabled library specifications (no compiler-owned data structures)

master + M5D-DEBUG-001
  -> M5D-DEBUG-002 interactive debugger (explicitly deferred)

M1 semantic services + M3A graph/interfaces
  -> M5B-LSP-018 closed-module workspace navigation

M1F + existing M2A flow facts
  -> M2A-FLOW-022 decision, then one admitted behavior slice

M3B cache/product boundary
  -> M3B-BOUNDARY-002 only after explicit fallback policy

feat/m5c-repl
  -> M5C-REPL-001 re-audit, outside the active queue for now

master + M1 semantic metadata + existing linear register IR
  -> M7-IR-SSA-001 design, CFG foundation, SSA shell, dominance analysis,
     phi placement, and binding/effect contract (branch-only)
  -> branch-only internal O1 copy/phi simplification and pure DCE
  -> default-pipeline O1 only after the debug/cache decisions and focused
     semantic parity corpus are admitted
```

M6-LANG-HASH-001 and M6-LANG-OPERATOR-001A..001C are complete. The
M7-IR-SSA-001 implementation slice is now integrated on `master`; its default
O1 policy and broader optimization work remain explicitly deferred. The
M7-LANG-OPTIONAL-001 implementation is complete on its focused feature branch;
its compatibility deletion remains a later decision. M5D-DEBUG-002 and the
other entries remain deferred specifications with clear future boundaries.
They do not reopen completed work or authorize concrete data-structure
implementations in the compiler repository.

## Verification contract

Before every development slice, rebuild from the current checkout. A stale
`build/compiler_design` can make a completed feature appear broken, as happened
when the formatter corpus was run before rebuilding the merged private-field
parser.

Before claiming a slice complete, run the repository gate:

```sh
python3 tests/verification_inventory.py
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
python3 tests/run_boundary_tests.py ./build/compiler_design
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs --report build/malformed-report.json
ctest --test-dir build --output-on-failure
python3 tests/run_golden_tests.py ./build/compiler_design
python3 tests/run_golden_tests_selftest.py
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
python3 tests/lsp_tests.py ./build/compiler_design
python3 tests/debugger_tests.py ./build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
cargo test --manifest-path vm-rs/Cargo.toml
rm -rf tests/__pycache__
```

The inventory revision, baseline commit, exact commands, toolchain, and
measured output must be recorded in the relevant decision/verification record.
Generated build outputs and Python caches are not roadmap evidence and must not
be committed.

## Conditional research track

The following are intentionally not in the active queue:

- recursive structs and richer nominal/structural type relationships;
- protocols, traits, dynamic dispatch, inheritance, or overloading;
- strings as iterable values and a general iterator abstraction;
- garbage collection, task scheduling, async execution, and JIT compilation;
- package registries, dependency solving, and large standard-library design.

Each research item requires a separate decision covering user value, language
semantics, module/interface impact, diagnostics, runtime representation,
workload, and verification before admission.

## Completion rule

The roadmap is successful when the active queue stays short, every proposed
slice has a named proof, and completed work is removed from the queue instead of
being mistaken for future development. The source contract remains in the
language documentation and tests; this file records only what still needs a
decision or implementation.
