# Compiler Design Roadmap

This is the active planning document for the compiler and language. It is
organized around capabilities, migration boundaries, and measurable outcomes
rather than individual syntax forms or standard-library functions. The roadmap
was re-baselined into this execution plan on 2026-07-22.

The earlier numbered feature phases remain useful history, but they are not a
queue to continue indefinitely. `docs/roadmap-archive-2026-07-04.md` and
`docs/roadmap-backup-2026-07-10.md` are historical snapshots rather than a
complete ledger of every intermediate roadmap edit; later superseded states
remain available in Git history. The implemented language contract remains in
`README.md` and `docs/language-grammar.ebnf`, and the disposition of major
still-open areas from the previous roadmap is recorded below.

## Why the roadmap changed

The project has accumulated a broad and useful language surface: functions and
closures, generic types, enums and exhaustive matching, mutable collections,
named structs and methods, source imports, bytecode artifacts, and a Rust VM.
The marginal value of another isolated helper or syntax feature is now lower
than the value of improving the foundations shared by every future feature.

The current question is no longer only whether the language can grow. It is
whether it can grow with low repetition, low regression risk, and a verification
story that remains understandable as the implementation expands. The current
bottleneck is integration cost: type checking, name resolution, flow facts,
module metadata, and AST-to-IR lowering still meet in a large front-end
boundary. Source imports are already lexed and parsed per file and represented
as module AST nodes, but type checking and lowering still consume a whole-program
AST containing dependency bodies. Module interfaces are emitted but are not yet
the compilation input for importers. The C++ compiler, `.cdbc` format, and Rust
VM have broad parity coverage, but their compatibility and debugging contracts
can be made more explicit.

From this point forward, a small user-visible feature is a means of validating
one of these capabilities, not a roadmap item by itself.

## Current baseline

The repository currently provides:

- a C++17 lexer, parser, type checker, register IR, and bytecode emitter;
- source locations and file-aware front-end diagnostics;
- lexical scopes, functions, closures, generic functions/enums/structs,
  nullable types, arrays, maps, ranges, named structs, methods, imports, and
  exhaustive pattern matching;
- a line-oriented, versioned `cdbc 0.1` compiler/VM contract with deterministic
  core sections, optional debug metadata, a standalone Rust VM, end-to-end
  execution, and artifact-parity tests;
- module-interface metadata, import search paths, re-exports, warnings and
  sanitizer build options, and a broad golden-test suite.

The main architectural constraints are:

- the type checker is the largest front-end hotspot and owns several unrelated
  responsibilities;
- the type checker and IR compiler independently interpret the AST, so semantic
  decisions can be duplicated across passes;
- module interfaces exist, but importers still consume dependency AST bodies;
  interfaces do not yet form a complete separate-compilation or incremental-
  build model;
- diagnostics, point locations, runtime stack information, and artifact metadata
  are available in pieces rather than one consistently shared full-range source
  contract;
- formatter, REPL, language-server, and source-level debugging workflows are
  not yet productized.

## Execution model

Every milestone or vertical slice in this document must have four parts:

1. **Deliverable:** a boundary that can be merged, reviewed, and reverted on
   its own.
2. **Migration strategy:** how the new path coexists with the old path, how
   parity is checked, and how the cutover is performed.
3. **Quantitative gate:** a measurable target and a command or fixture set that
   proves it.
4. **Old-path deletion condition:** an explicit condition for removing the
   compatibility branch, duplicate implementation, or temporary adapter.

The old path is removed because the new path is proven, not because a refactor
has become inconvenient to maintain. During migration, run old and new paths in
shadow mode where possible and compare semantic output or generated IR; do not
execute a program twice merely to compare runtime side effects.

Before a slice begins, its design/plan binds every percentage or zero-count gate
to a machine-readable inventory revision, baseline commit, measurement command,
and recorded CI output. A comparison also defines canonicalization and allowed
differences. Performance gates additionally record the toolchain, build mode,
host characteristics, repetition policy, comparison statistic, and tolerance;
measurements from rolling CI images may be published as observations but are
not reproducibility claims.

In this document, `100%` means every in-scope case ID in the named inventory
revision; `zero unexplained differences` permits only reviewed differences with
an explicit recorded disposition.

Each semantic slice must preserve the existing end-to-end chain. In this
document, HIR means the typed higher-level semantic representation introduced
by M1:

```text
source -> lexer/tokens -> parser/AST -> semantic model/HIR
       -> register IR -> bytecode program -> .cdbc
       -> Rust artifact parser -> Rust VM
```

When a slice changes behavior across that chain, its proof includes the
relevant AST/IR/bytecode output, diagnostics, runtime goldens, artifact parser,
and Rust VM tests. Error fixtures are proved at the stage they are expected to
reach; a parse or import-loading failure is not required to enter semantic
analysis. A slice is not complete because one compiler binary accepts the
source.

## Prioritization model

Work should be selected by the outcome it unlocks, not by the novelty of its
surface syntax.

| Priority | Capability outcome | Why it comes first |
| --- | --- | --- |
| P0 | Verification foundation | Makes regressions, cross-backend mismatches, and malformed-input failures cheap to detect. |
| P0.5A | Semantic decision baseline | Removes ambiguity before HIR work. |
| P0.5B | Existing artifact audit and evolution decision | Establishes what `cdbc 0.1` already guarantees and what a justified successor would need. |
| P1A | Shared semantic front end | Removes duplicated type/name/lowering decisions and makes future features cheaper. |
| P1B | Coherent language semantics | Turns scattered type and control-flow rules into one predictable model. |
| P2A | Module graph and project model | Enables real reuse, separate compilation, and incremental work across files. |
| P2B | Formal artifact and runtime contract | Makes the C++ compiler and Rust VM independently evolvable and debuggable. |
| P3 | Independent developer tools | Delivers formatter, LSP, REPL, and debugger on their own dependency schedules. |
| Conditional | Advanced language and VM research | Starts only when a concrete use case and the preceding contracts justify it. |

The dependency shape is a directed graph, not one mandatory serial queue:

```text
M0A fixture inventory + canonical command
  |----> M0B boundary comparison
  |----> M0C deterministic malformed-input coverage
  |----> M0D verification matrix + measurement baseline
  |----> M0.5A semantic decisions
  `----> M0.5B cdbc 0.1 audit

M0B + M0.5A ------------------------------> M1 shared semantic front end
M1F --------------------------------------> M2A coherent semantics/dataflow
M0C --------------------------------------> M2B parser recovery
M1F --------------------------------------> M2B type/semantic recovery
M1F --------------------------------------> M3A module graph/interfaces
M1A1 -------------------------------------> M1A2 lossless token/trivia view
M0D --------------------------------------> M1F final cutover gate
M0B + M0C + resolved M0.5A runtime-ABI decisions + M0.5B
  ------------------------------------------> M4A artifact validation/compatibility
M3A + an explicit module-artifact decision ----> M3B separate/incremental compilation
M1A1 source identities + M3A module identities + M4A --> M4B debug/runtime metadata
```

M3B depends on M4A only if separately compiled or cached module products use
`.cdbc`; it may instead link an internal module object into one final program
artifact. That choice must be made explicitly before M3B begins. Recommended
schedule and hard dependencies are kept distinct below.

## Milestone 0: Verification foundation

**Objective:** make the current pipeline easy to verify, compare, and evolve.

M0 is test and observability infrastructure. It adds no compatibility promise
beyond the existing `cdbc 0.1` contract; supported-version ranges, negotiation,
and migration rules belong to Milestone 4.

### M0A: Fixture inventory and canonical verification command

**Deliverable:** add a machine-readable inventory that assigns a stable
`case_id` to each independently measured check and records its source fixture or
suite, expected stage, capability tags, backend, and expected-result kind. A
source fixture may support several case IDs when different stages or backends
are asserted independently; non-enumerable unit-test binaries are represented
as named suites. Provide one stage-aware verification command that extends or
orchestrates the existing CTest and specialized runners without reimplementing
their assertions.

**Migration:** keep the current direct commands and CI steps as the reference.
Run the canonical and legacy orchestration against the baseline inventory and
compare case discovery, pass/fail/skip classification, exit status, and failure
text before changing compiler behavior.

**Quantitative gate:** every independently measured baseline check has exactly
one stable case ID in the inventory, while one source fixture may intentionally
produce multiple stage/backend checks; every entry is reachable from the
canonical command; canonical and legacy orchestration have zero unexplained
discovery or classification differences; and CI stores the machine-readable
result.

**Delete the old path when:** the canonical command passes the same baseline in
two consecutive CI runs and documents how a new stage, capability, or backend
is registered. Delete only duplicate top-level orchestration; retain specialized
runners as components while they still own useful focused assertions.

### M0B: Boundary comparison and failure reports

**Deliverable:** report the first failing or disagreeing boundary across
tokens/AST, semantic diagnostics or dumps, register IR, bytecode program,
canonical `.cdbc`, Rust decoding/validation, and VM output. Record a pre-M1
reference commit plus canonical semantic/IR/artifact/runtime outputs so later M1
slices can delete production legacy code without losing the final comparison
oracle.

**Migration:** add boundary capture beside existing final-output checks. Compare
deterministic intermediate output rather than executing side-effecting programs
twice, and keep canonicalization and permitted differences in a reviewed
machine-readable allowlist.

**Quantitative gate:** every multi-boundary capability in the M0A inventory has
an expected boundary sequence; injected mismatches at each supported boundary
are attributed to that boundary; and the bound M0A baseline inventory has zero
unexplained old/reference versus current differences.

**Delete the old path when:** boundary-aware reports cover every existing final-
only category for two CI runs. The checked-in reference outputs remain until M1F
finishes even after the production implementation they describe is removed.

### M0C: Deterministic malformed-input and property coverage

**Deliverable:** deterministic property/fuzz harnesses for the lexer, parser,
diagnostic recovery, and `.cdbc` parser, with fixed seed corpora, bounded input
size and execution time, and promotion of minimized failures into regression
fixtures.

**Migration:** introduce the harnesses without changing accepted language or
artifact behavior. Run them first as bounded CI jobs and record seed, tool
version, budget, and failing input for every run.

**Quantitative gate:** the named seed corpus completes within the declared
budget with zero crashes, sanitizer failures, hangs, or uncontrolled resource
exhaustion; the same seed and toolchain reproduce the same case set; and a
harness self-test proves that a known injected failure is minimized and saved.

**Delete the old path when:** no compiler or VM production path is deleted for
this slice. Remove a temporary fuzz adapter only after the canonical harness
reaches the same target with the same seed corpus and failure reproduction.

### M0D: Verification matrix and measurement baseline

**Deliverable:** one matrix covering warning-enabled builds, sanitizers, C++
tests, Python goldens, artifact tests, Rust VM tests, and `cargo test`, plus a
versioned baseline record for compile time, test duration, artifact size, and a
named runtime workload set. The record includes commit, commands, corpus,
toolchain, build mode, host characteristics, repetition policy, comparison
statistic, and tolerance.

**Migration:** preserve the current CI jobs while the canonical command is
introduced, but mark duplicated executions so test duration is counted once.
Separate deterministic size/count gates from noisy performance observations;
rolling hosted-runner measurements remain informational unless the environment
and tolerance make them comparable.

**Quantitative gate:** every matrix cell is reachable from the canonical command
or an explicitly named environment-specific wrapper; a clean checkout produces
the complete baseline record; deterministic fields reproduce byte-for-byte;
and performance fields satisfy their documented sample and tolerance policy.

**Delete the old path when:** duplicate CI steps are removed only after the
canonical matrix has equivalent coverage and failure diagnostics in two
consecutive CI runs. The baseline schema remains versioned so later milestones
can compare against the exact M0D environment instead of an unnamed machine.

## Milestone 0.5: Architecture decision baselines

**Objective:** record the semantic decisions that HIR depends on and audit the
existing artifact contract before either boundary is redesigned. M0.5A and
M0.5B both consume the M0A inventory, but neither blocks the other.

### M0.5A: Semantic decision log

Status: implemented in `docs/decisions/m05a-semantic-decisions.json` with the
reproducible source-site baseline in `docs/verification/m05a-baseline.json`.

**Deliverable:** create a small, versioned decision log covering at least:

- evaluation order, side effects, short-circuiting, and error propagation;
- binding identity, lexical scope, closure capture, shared cells, and mutation;
- type identity, assignability, unknown/dynamic values, generic substitution,
  function compatibility, and variance policy;
- source-file ownership, full-range/coordinate conventions, diagnostic
  ownership, lossless token/trivia needs, and source-to-artifact mapping;
- module identity, direct CLI root grouping, import-cycle rejection, visibility,
  re-export behavior, and interface ownership;
- pattern bindings, guards, exhaustiveness, reachability, and nullable cases;
- the semantic boundary between native calls, IR operations, bytecode, and VM
  runtime behavior.

Each entry records a stable decision ID, rationale, affected consumers,
compatibility consequences, and one of `resolved`, `open`, or `deferred`.
`open` and `deferred` entries state which named milestone or slice, if any, they
block and what later decision will close them. Record a baseline inventory and
measurement command for duplicated type/name/lowering decision sites.

**Migration:** write down current behavior before designing HIR. M1 is a
behavior-preserving structural migration against this baseline; an intentional
semantic change belongs to M2 or to a separately accepted decision update.
Conflicting prose is not silently selected by the HIR implementation.

**Quantitative gate:** every listed domain has an entry and owner; every
M1-blocking entry is `resolved`; every M1 design or implementation slice cites
the decision IDs it consumes; the duplicated-decision inventory is reproducible
from its recorded command; and zero HIR changes rely on an undocumented
semantic choice. Marking a blocking item merely `deferred` does not satisfy the
gate.

**Delete the old path when:** this documentation slice deletes no production
path. A conflicting local semantic rule may be removed only by its M1 or M2
slice after the shared implementation and compatibility evidence exist.

### M0.5B: `cdbc 0.1` contract audit and evolution decision

Status: implemented in `docs/decisions/m05b-cdbc-contract.json` with the
reference audit in `docs/verification/m05b-baseline.json`; no successor version
is selected at this stage.

**Deliverable:** treat the existing line-oriented `cdbc 0.1` format as the
current versioned compiler/VM contract. Inventory what it already provides:
the `cdbc 0.1` family/version header, canonical core section order, strict Rust
parsing/formatting, and optional debug metadata. Classify each proposed addition
as `already present`, `compatible-extension candidate`, `successor-version
candidate`, or `not currently required`, and decide whether M4A needs a
successor at all. Candidate additions include artifact kind, runtime/target
identity, capability flags, module identity, unknown-section policy, explicit
framing, and integrity metadata; fields without a concrete consumer or
transport requirement remain deferred rather than becoming speculative format.

M0.5B does not erase the existing documented promise that a future change is
compatible with `0.1` or uses a new version number. M4A later defines supported
version ranges, negotiation, migration/rejection policy, and the support
lifecycle for older versions, if any.

**Migration:** make no artifact reader/writer or default-emission change in
M0.5B. Use the current `0.1` reader, writer, canonical dumps, diagnostics, and
execution results as the M4A reference corpus. Record the compatibility
direction and required fixtures for every selected extension or successor
proposal.

**Quantitative gate:** every artifact case ID in the M0A inventory records its
current version and existing envelope capabilities; current `0.1` parse/dump is
byte-for-byte deterministic for the reference corpus; current invalid
family/version cases fail before VM execution; every field selected for a
compatible extension or successor version has a named consumer and planned
fixture; every deferred or not-currently-required field records its rationale
and revisit condition; and the successor/no-successor decision has a reviewed
rationale. Conversion, rejection, and supported-version policy remain M4A
outputs rather than being decided here.

**Delete the old path when:** M0.5B deletes no artifact path and does not demote
the current `0.1` reader/writer. M4A exclusively owns implementation, cutover,
and any older-version deletion after its compatibility matrix and migration/
rejection policy are checked in.

## Milestone 1: Shared semantic front end

**Objective:** establish one semantic model that the backend can consume rather
than making each later pass rediscover meaning from raw AST nodes.

M1 is not one large rewrite. It is a sequence of independently deliverable
vertical slices. M1 preserves the M0.5A semantic baseline while changing the
representation and ownership of decisions; new language behavior belongs to M2
or a separately accepted decision update. Each slice migrates one semantic
family end to end, proves reference and new behavior in parallel, and deletes
its duplicate production path before the next slice expands the boundary. The
M0B reference corpus remains available after those deletions for M1F.

### M1A: Identity and source foundations

M1A contains two independently reviewable slices. M1B depends on M1A1, not on
formatter-oriented trivia work in M1A2.

#### M1A1: Snapshot identities and source ranges

Current implementation slice: `include/SourceIdentity.hpp`, token/AST range
metadata, `ResolvedNames` binding/scope IDs, and direct multi-file diagnostic
range coverage are documented in `docs/source-metadata.md`. The existing
point-span, string lookup, CLI diagnostic, and artifact paths remain in place
until their later migration gates are satisfied.

**Deliverable:** introduce domain-typed, snapshot-stable identities for source
files, syntax nodes, declarations/symbols, and bindings, plus complete byte
ranges anchored to source IDs and documented line/column conversion. Each
identity domain declares its owner and lifetime. Nominal type references use
declaration IDs while canonical type data belongs to M1E; cross-build module/
export/cache keys belong to M3A/M3B, and serialized artifact/debug identities
belong to M4A/M4B. The first proof covers lexical bindings, variable reads,
assignments, block scope, and file-aware diagnostics.

**Migration:** retain current string/pointer lookups and point locations while
recording snapshot IDs and ranges beside them. Shadow-compare binding resolution
and existing diagnostic output. Do not serialize or cache snapshot-local IDs as
persistent keys.

**Quantitative gate:** every variable, assignment, scope, and direct-multi-file
case ID selected from the bound M0A inventory uses snapshot IDs; within a
snapshot each migrated entity has one ID and no persistent cache or artifact
serializes it; semantic resolution and old lookup disagree zero times; every
migrated token/node range is valid and ordered; and diagnostics retain their
line/column/path output.

**Delete the old path when:** no migrated binding or diagnostic reaches the
legacy lookup or point-location-only source representation through production
code, and the named proof cases pass through the canonical verification command.

#### M1A2: Lossless token and trivia source view

Current implementation slice: `LosslessSourceView` is built from the production
token ranges and original source bytes, then exposed by
`FrontendSession::losslessSourceView()`. Its focused tests cover exact
round-trip, comment placement, comment text inside strings, and direct
multi-file source grouping.

**Deliverable:** introduce a lossless token/source view that preserves comments
and other trivia exactly once, anchored to M1A1 source IDs and ranges, without
creating a second lexer or grammar.

**Migration:** produce the lossless view beside the existing token stream and
AST. Keep compiler semantics on the existing path while round-trip and comment-
placement cases prove the new source representation.

**Quantitative gate:** every case ID in the bound lossless-source corpus
reconstructs its original bytes; all token/trivia ranges are valid, ordered, and
non-duplicated; every comment is retained exactly once with unchanged text and
order; and parser/semantic outputs have zero unexplained differences.

**Delete the old path when:** temporary trivia recovery or duplicate tokenization
has zero consumers and formatter/editor source consumers use the production
lossless view. M1A2 is not a gate for M1B or M1F.

### M1B: Declaration collection and name resolution

Current implementation slice: `DeclarationIndex` collects lexical declaration
records and scopes from the existing AST, including function signatures,
struct/enum declarations, method metadata, import/export records, namespace
aliases, variable/assignment references, and local direct-call or struct-method
targets, for-in bindings, and match pattern bindings. `TypeChecker` builds this
index in shadow mode and compares value-reference names, source ranges, binding
targets, and locally available call targets with its legacy `ResolvedNames`
result. Declaration signatures and struct/enum shapes are exposed as AST-backed
queries without claiming canonical type ownership. The proof slice is focused in
`tests/ir_source_location_tests.cpp`; native calls, enum constructors,
namespace-qualified calls, and imported method targets remain external cases,
while module graph construction and imported symbol materialization remain
scheduled for M3A.

**Deliverable:** separate declaration collection, lexical lookup,
imported/exported symbols, function signatures, struct/enum declarations, and
method metadata from expression checking. The proof slice covers declarations,
calls, methods, and namespace-qualified names without changing source syntax.

**Migration:** build the new symbol table from the existing AST, compare
resolved symbol IDs and signatures with the current `TypeChecker` results, and
switch one declaration family at a time. Keep module graph construction in M3A;
this slice only establishes the semantic interface it will consume.

**Quantitative gate:** every function, method, struct, enum, import, export, and
namespace case ID tagged in the M0A inventory passes through the new
declaration/resolution path; zero unresolved-symbol mismatches remain in shadow
mode; every resolved call has a snapshot-stable target ID before lowering.

**Delete the old path when:** each migrated declaration family has no fallback
name-resolution call site and its old symbol-table representation is no longer
read by IR lowering.

### M1C: Typed expressions and assignment targets

Current implementation slice: `DeclarationIndex` exposes snapshot-local
`TypedExpressionRecord` values for resolved variable reads, ordinary and
compound assignments, direct calls, field access, field assignments and field
compound assignments, index reads/assignments/compound assignments, and native
function/member calls. `TypeChecker`
materializes the existing `TypeInfo` result beside its legacy path, including
statically known and dynamically validated indexing paths plus array/map/struct
expression inference, and shadow comparison requires metadata completeness for
the migrated expression families. IR lowering now consumes native-call records,
typed call result records, local direct/member-call targets, and
variant-constructor records, variable, assignment, and compound-assignment
result records, array/map/struct type records, field-access and field-assignment
result records, and index result records while `len` keeps its legacy dedicated
path; collection helpers, control flow, and other semantic ownership remain
later M1C/M1D/M1E slices.

**Deliverable:** introduce typed semantic nodes for literals, variables, calls,
indexing, field access, assignments, compound assignments, and native calls. The
proof slice must cover both statically known and dynamic runtime-validation
paths.

**Migration:** type-check the existing AST once, materialize resolved targets
and types, and make IR lowering consume those results. Compare semantic dumps or
IR rather than executing programs twice.

**Quantitative gate:** every expression, assignment, collection, struct-field,
and native-call case ID tagged in the M0A inventory passes through typed nodes;
zero backend lookups re-infer a binding or call target for migrated nodes; and
comparison with the M0B IR reference has zero unexplained differences.

**Delete the old path when:** `IRCompiler` no longer performs type/name
resolution for a migrated expression family and the corresponding AST-specific
re-inference helpers have zero production callers.

### M1D: Control flow, functions, and closures

Initial implementation slice: `DeclarationIndex` retains named
function/method signatures, typed function-expression results, checked return
records, and capture records. IR lowering requires those records for named
functions, methods, anonymous functions, and returns while preserving the
existing runtime names and closure-cell representation. Capture records now
cover enclosing function-owned local cells reached by reads, assignments, and
compound assignments. Loop-target records now resolve `break` and `continue`
to the nearest loop across `while`, C-style `for`, and `for-in` forms while
respecting function boundaries.

**Deliverable:** represent control-flow edges, returns, loop targets, function
signatures, closure captures, and shared-cell accesses in the semantic layer.
The proof slice covers `if`, loops, `break`/`continue`, functions, recursion,
anonymous functions, and nested closures. These are behavior-preserving targets
and capture facts for lowering, not M2's dataflow/narrowing conclusions.

**Migration:** keep the current lowering as the reference implementation;
materialize capture sets and control-flow targets first; then lower the same
semantic representation to existing IR operations and compare artifacts and
Rust VM behavior.

**Quantitative gate:** every control-flow/function/closure case ID tagged in the
M0A inventory uses the new capture and control-flow metadata; every captured
binding has one snapshot-stable ID; C++/Rust VM parity has zero regressions; and
runtime diagnostics and source-mapped frames in the bound runtime case set have
zero unexplained differences from the M0B reference.

**Delete the old path when:** capture discovery and loop-target resolution are
performed only by the semantic layer, with no AST re-walk in the backend for
migrated constructs.

### M1E: Types, generics, aggregates, and patterns

M1E is an umbrella for three separately merged and reverted slices, not one
checker rewrite. Each slice preserves runtime representations and bytecode
operations and uses the M0B reference corpus as its compatibility oracle.

#### M1E1: Type core and generics

Initial implementation slices: checked named function and method declarations
publish canonical `TypeInfo` signatures, including resolved parameter and
return types, generic parameters, constraints, and implicit method receivers;
IR lowering consumes these records. The shared `SemanticTypes` interface now
also owns type-identity predicates, compatibility, aggregate element-type
merging, recursive generic substitution, structural generic inference, and
type-parameter constraint validation, while the existing algorithms remain the
behavior oracle.

**Deliverable:** move type identity, assignability, nullable compatibility,
function compatibility, type-parameter bounds, inference, and generic
substitution behind the shared semantic type interface.

**Migration:** route one type utility or generic instantiation family at a time
through the shared model while shadow-comparing current results and diagnostics.
The current slices centralize type-identity predicates, compatibility,
collection element merging, type-parameter substitution, structural generic
inference, and constraint validation in `TypeUtils`; `TypeChecker` uses the
namespaced operations and keeps only the diagnostic/context adapter around
them. The old free functions only forward to the shared implementations.

**Quantitative gate:** every type-core/generic capability named by the M0A
inventory is classified by the shared model; every generic instantiation used
by lowering has explicit substitutions; and reference/new semantic or IR
comparison has zero unexplained differences.

**Delete the old path when:** migrated compatibility/substitution helpers have
zero production callers outside the shared type model.

#### M1E2: Collections and nominal aggregates

Initial implementation slice: `DeclarationIndex` records resolved collection
index operations for array, map, range, and dynamic paths, including read,
assignment, and numeric compound-assignment kinds; IR lowering consumes these
records while preserving the existing erased runtime values and opcodes.

**Deliverable:** represent arrays, maps, ranges, structs, enums, fields,
variants, and resolved aggregate operations in HIR.

**Migration:** migrate one aggregate family at a time while preserving erased
runtime representations, mutation/aliasing behavior, and existing bytecode.

**Quantitative gate:** every aggregate capability named by the M0A inventory
uses resolved semantic types and operations; the backend performs zero
AST-shape reconstruction for migrated families; and artifact/VM comparison has
zero unexplained differences.

**Delete the old path when:** local aggregate type/reconstruction helpers have
zero production callers and IR lowering consumes resolved HIR operations.

#### M1E3: Patterns and coverage inputs

**Deliverable:** move literal, nullable, enum, record, OR-pattern, guard,
binding, and exhaustiveness inputs behind the shared semantic pattern interface.

**Migration:** migrate one pattern family at a time while retaining the current
coverage result and lowering as the behavior oracle.

**Quantitative gate:** every pattern capability named by the M0A inventory is
classified by the shared model; bindings and substitutions are explicit; and
semantic, diagnostic, IR, artifact, and VM comparison has zero unexplained
differences. No opcode is introduced solely for this migration.

**Delete the old path when:** local pattern compatibility/coverage inputs have
zero production callers and lowering consumes resolved pattern operations rather
than reconstructing them from AST shape.

### M1F: Front-end cutover and cleanup

**Deliverable:** make the semantic representation the only input to register-IR
lowering, retain the AST printer only for syntax inspection, and remove
migration-only adapters.

**Migration:** compare the final result with the canonical M0B reference corpus,
switch every type-checking and lowering CLI/test entry point to HIR, and retain
stage-aware routing for lexer, parser, and import-loading failures. Land final
semantic-path deletion as a separate reviewable change so rollback remains
possible.

**Quantitative gate:** every in-scope type-error, success, runtime-error,
artifact, and Rust VM case ID that reaches semantic analysis uses HIR;
lexer-error, parse-error, and load-time import-error case IDs retain their stage
classification, diagnostics, stdout behavior, and exit status without being
forced into semantic analysis; production legacy semantic-path invocation count
is zero; the M0D verification matrix is green; and duplicated semantic decision
sites for migrated families are zero or appear in an explicit owned exception
inventory.

**Delete the old path when:** the gates above hold for two full verification
runs, the rollback/reference point is recorded, and no M1 semantic migration
adapter remains in backend-producing CLI or test paths. Module-loading and
artifact-version adapters are governed by M3A/M3B and M4A and are not M1F
blockers.

## Milestone 2: Coherent language semantics and analysis

**Objective:** make the language rules internally consistent before expanding
the surface area again.

Unlike M1's behavior-preserving representation migration, M2A may intentionally
change language semantics, but only through an explicit decision update with a
migration note and positive/negative compatibility fixtures. M2B may change
diagnostic aggregation or recovery only through an explicit diagnostic decision
with ordering, output, and exit-status compatibility fixtures.

M2A and M2B's type/semantic recovery consume M1's name, type, pattern, and HIR
interfaces; they do not reopen their ownership or create a second semantic
model. M2B's parser recovery remains a syntax-stage service and does not consume
HIR.

### M2A: Semantic consistency and dataflow

**Deliverable:**

- use M1's shared type model to identify and close cross-context inconsistencies
  among type compatibility, generic substitution, function arity/compatibility,
  nullable values, collection element types, struct fields, enum payloads, and
  pattern bindings;
- build a sound flow-analysis framework for branch, loop, return, mutation, and
  closure boundaries, including invalidation when aliases or calls can mutate
  state;
- share exhaustiveness and reachability reasoning between match statements,
  match expressions, nullable cases, guards, and future pattern forms;
- consolidate M0.5A decisions and any accepted M2 revisions for evaluation
  order, aliasing, closure capture, mutation, runtime equality, and dynamic-value
  checks into one normative language-semantics document;
- decide recursive type support, richer narrowing, and remaining known
  limitations as deliberate language policy rather than incremental TODOs.
  Richer loop/post-branch/field/index narrowing is intentionally reopened for
  decision here; existing behavior remains the contract until such a decision
  is accepted. M2A may decide recursive-type policy, but implementation still
  requires a separately admitted behavior-change slice.

**Migration:** add one analysis rule at a time on top of M1's semantic model.
Keep existing special cases as assertions or compatibility shims during
migration, cite the M0.5A decision being preserved or revised, and add positive,
negative, mutation-invalidation, and recovery fixtures before changing the
default result.

**Quantitative gate:** every statement, expression, pattern, and type form in
the implemented grammar appears in the versioned analysis inventory with a
corresponding M0A capability label; every flow-fact case ID in the named analysis
inventory is produced by the shared dataflow engine, with zero production-only
ad-hoc narrowing rules outside it; and compiler artifact output and Rust VM
behavior have zero unexplained semantic differences across the named M0A parity
cases and M0B references.

**Delete the old path when:** remove local `TypeChecker`/`FlowFacts` special
cases only after their rule has a shared analysis implementation, a mutation-
invalidation test, and zero callers outside the shared engine. Do not delete a
conservative rule merely to make a new feature type-check.

### M2B: Front-end diagnostic recovery

**Deliverable:** report independent lexer/parser/type diagnostics without
corrupting later analysis while retaining distinct lexer, parser, type,
import-loading, compile, and runtime error categories. Parser recovery remains a
syntax-stage service and does not consume HIR; it may proceed after M0C.
Type/semantic recovery consumes HIR after M1F.

**Migration:** add one recovery boundary at a time with accepted, rejected,
multi-error, ordering, and resynchronization cases. Keep stop-first behavior as
the reference for categories not yet migrated, and never continue into a later
compiler stage after an earlier stage is invalid.

**Quantitative gate:** every documented diagnostic category has named accepted
and rejected case IDs where both outcomes apply; repeated runs produce identical
diagnostic order, ranges, stdout, and exit status; injected independent errors
are all reported up to the declared recovery limit; and later diagnostics never
depend on corrupted parser or semantic state.

**Delete the old path when:** a migrated category has no production stop-first
or local-resynchronization fallback outside the shared recovery service and its
bound M0A/M0C cases pass the canonical command. Parser recovery does not wait for
M1F; type/semantic recovery does.

## Milestone 3: Module graph and project model

**Objective:** turn imports from source-loading conveniences into a dependable
module/build boundary.

The current loader already lexes and parses imported files as individual
`ParsedUnit` values and assembles `ModuleStmt` nodes. The legacy boundary for M3
is therefore whole-program dependency-body checking/lowering, not dependency
source concatenation. Direct CLI roots remain one ordered entry program under
the current documented semantics: the no-import fast path parses combined input,
while the import-aware loader uses per-file module nodes. Neither behavior is a
legacy dependency-import path, and changing their source/visibility/diagnostic
contract requires a separate language/CLI decision.

### M3A: Explicit graph and interface-driven semantics

**Deliverable:** evolve `FrontendSession` into an explicit graph with module
identities deterministic across equivalent builds, dependency edges, source
origins, visibility, and current-policy cycle detection/rejection. Make exported
symbols, generic signatures, struct methods, enum metadata, and relevant
diagnostics first-class versioned module-interface data. Define project roots,
entry modules, import resolution, search paths, and re-exports without ad-hoc
filesystem scanning; package registries and dependency solving remain out of
scope.

**Migration:** build graph/interface consumption beside the existing
`ParsedUnit`/`ModuleStmt` whole-program body path. For source imports, namespace
imports, re-exports, search paths, and cycles, compare graph-derived visibility,
types, diagnostics, and final artifacts with the current result. Preserve direct
single-file and ordered direct-multi-file entry-program modes, including
file-local diagnostic remapping.

**Quantitative gate:** every import/export/namespace/re-export/search-path/cycle
capability named by the M0A inventory uses graph-derived module identities;
after a dependency interface is produced, importer name/type analysis reads no
dependency source or AST body; interface order and file-aware diagnostics are
byte-for-byte stable across repeated equivalent builds; current import cycles
remain deterministically rejected; and direct-multi-file scope, order, and
diagnostics retain their baseline output.

**Delete the old path when:** all imported-file and diagnostic-remapping checks
use graph/interface semantics and name/type/visibility consumers have zero
fallback reads of a dependency body. M3B exclusively owns removal of dependency-
body lowering. Preserve the documented direct-input entry-program adapter; it
is not a legacy import path.

### M3B: Separate compilation and incremental rebuilds

**Deliverable:** compile module bodies independently, define dependency-aware
cache keys and invalidation, and retain a clear single-file/program mode. Before
implementation, decide whether cached/linked module products are internal
objects combined into one final `.cdbc` program or versioned per-module `.cdbc`
artifacts. The latter choice depends on M4A's artifact kind, link/load, and
runtime compatibility contract.

**Migration:** produce independent module results and cache decisions in shadow
mode while the current whole-program lowering remains authoritative. Compare
interfaces, linked program artifacts, diagnostics, and execution before enabling
cache reuse by default.

**Quantitative gate:** on the named incremental-build graphs in the M0A
inventory, a no-change rebuild reuses every unchanged dependency interface and
eligible compiled result; an implementation-only leaf change with an unchanged
public interface recompiles that leaf but zero dependents; a public-interface
change recompiles the leaf and its transitive dependents but zero unrelated
modules; repeated builds emit canonical-equivalent final artifacts; and the
measurement report names every rebuilt/reused module and reason.

**Delete the old path when:** all module-graph fixtures compile through
independent results, cache invalidation passes the declared change matrix, and
no import build performs whole-program dependency-body type checking or lowering
as a fallback. Keep deliberately documented single-entry adapters that consume
the same graph and interfaces.

Package management and a large standard library are later product decisions.
They should consume this module boundary rather than define it.

## Milestone 4: Formal artifact and runtime contract

**Objective:** make the already-versioned compiler/VM boundary explicitly
compatible, validated, observable, and safe to evolve. M0 supplies verification;
M0.5B audits `cdbc 0.1`; M4 owns the long-term compatibility and deletion policy.

### M4A: Artifact validation, compatibility, and runtime ABI

**Deliverable:** publish a `.cdbc` compatibility matrix covering `0.1` and every
successor selected after the M0.5B audit: supported version ranges, major/minor
rules, capability negotiation, deterministic encoding, and actionable rejection
of unsupported artifacts. Centralize bytecode validation before execution with
matching C++ emitter and Rust parser/formatter tests. Define migration or
rejection policy for `0.1` and successor artifacts, and document/test the runtime
ABI for shared cells, closures, references, mutable collections, native calls,
and failure propagation, after the relevant M0.5A decisions are resolved. If M3B
selects per-module `.cdbc` products, M4A also defines artifact kinds, module
identities, link/load rules, and validation for the module-product set before
that M3B path begins.

**Migration:** use the M0.5B audit result as the reader/writer baseline. If no
successor is needed, harden `0.1` without inventing an adapter. If a successor is
needed, retain an explicit `0.1` reader, add the successor reader/writer beside
it, and compare decoded programs, canonical dumps, validation diagnostics, and
execution before changing the default emitter. Publish the compatibility matrix
before removing any version.

**Quantitative gate:** every supported family/header/version/capability
combination is accepted or rejected by an explicit rule; unversioned artifacts
remain rejected; every malformed-artifact case in the M0C corpus named by the
M0A inventory fails before VM execution; canonical emission is byte-for-byte
deterministic for the named artifact corpus; and each runtime ABI invariant has
matching parse/validation/execution coverage.
When per-module `.cdbc` is selected, every module-product kind and link/load
rule also has a compatibility and malformed-input case.

**Delete the old path when:** remove an older artifact parser/emitter only after
the compatibility matrix, migration/rejection behavior, deprecation period, and
all affected artifact fixtures are checked in and the canonical verification
command passes. M0.5B cannot delete `0.1` support on M4A's behalf.

### M4B: Module-aware source/debug and runtime metadata

**Deliverable:** standardize metadata so runtime errors, stack traces, and future
debug events map to stable module/source identities, functions, calls, and full
source ranges. M4B consumes M1A1 ranges/identities, M3A module identities, and
M4A's validated artifact representation rather than creating parallel tables.
It maps snapshot-local M1A1 source IDs to artifact-local serialized IDs; it does
not persist compiler snapshot IDs directly.

**Migration:** emit shared metadata beside current `debug_sources` and
`debug_locations`, compare runtime diagnostics and frame order, then switch the
VM and tools one metadata consumer at a time. Metadata-free artifacts retain
their explicitly documented fallback behavior while their supported versions
remain in the compatibility matrix.

**Quantitative gate:** every frame in the named M0A source-level runtime corpus
maps to a module, function, and valid source range; repeated execution produces
deterministic frame/event order; compiler-emitted runtime diagnostics use zero
fallback source guesses; and metadata-free behavior matches its compatibility
rule.

**Delete the old path when:** temporary runtime/source-map adapters have zero
consumers, all compiler-emitted frames use the shared metadata path, and the
bound C++/Rust conformance case set passes the canonical command.

Backend optimization work follows M4A and the M0D baseline. Constant folding,
control-flow optimization, register allocation, or another optimization enters
the roadmap only with a correctness fixture and a measured improvement against
the recorded environment. Memory ownership and garbage collection remain a
conditional track until allocation and root requirements are measured.

Task scheduling, async execution, and JIT metadata remain research topics until
the runtime contract and representative workloads justify them.

## Milestone 5: Independent developer-tool schedules

**Objective:** make the language practical to read, edit, experiment with, and
debug using the same compiler services as the CLI. These are separate products,
not one combined “toolchain” phase.

### Milestone 5A: Formatter

**Dependency:** the production lexer/parser and grammar plus M1A2's lossless
token/trivia view and complete source ranges.

**Deliverable:** build a syntax-aware formatter with documented formatting
stability. It should operate on lossless syntax, preserve comments and parse
meaning, and avoid creating a second tokenizer or grammar.

**Migration:** first make lossless token/syntax round trips observable through
the production front end; then build formatting on the same lexer/parser and
grammar. Keep examples and golden inputs, including comment-placement cases, as
the named formatter corpus. Define whether invalid/incomplete input is rejected
unchanged or supported before exposing editor formatting.

**Quantitative gate:** the lossless front-end round trip reconstructs every case
in the named formatter corpus registered in the M0A inventory before formatting;
every range is valid and ordered;
every comment is retained exactly once with unchanged text and order;
`format(format(source)) == format(source)` for that corpus; every formatted
input parses; and formatted programs have zero AST/semantic-output differences
from their unformatted forms.

**Old-path deletion condition:** delete formatter-specific tokenization, trivia
recovery, parser, or grammar code once all formatter inputs use the production
lossless syntax path and the range/comment/idempotence gates pass.

### Milestone 5B: Language Server

**Dependency:** M1 semantic identities and M3A module graph; M2 diagnostics make
the service useful but need not be completely finished before a prototype.

**Deliverable:** expose diagnostics, symbol lookup, definition/references, type
information, completion, and module navigation through a language-server-
compatible service.

**Migration:** make the CLI and LSP consume the same source, semantic, module,
and diagnostic services. Do not reimplement name resolution or type inference
inside editor adapters.

**Quantitative gate:** on the named multi-file/LSP corpus registered in the M0A
inventory, 100% of exported symbols and declarations have snapshot-stable
semantic IDs and valid definition ranges; cross-build exports use M3A keys rather
than snapshot IDs; diagnostics match the CLI in message, severity, and range;
and repeated queries reparse zero unchanged modules outside the documented cache
policy.

**Old-path deletion condition:** remove editor-specific diagnostic and symbol
resolution code when all supported LSP queries use shared semantic IDs and
module interfaces.

### Milestone 5C: REPL / incremental evaluation

**Dependency:** M3A module/session boundaries, M3B incremental compilation where
file-backed evaluation needs it, and the M4A/M4B runtime and metadata contracts.

**Deliverable:** add a REPL or incremental evaluation mode with explicit session
state, imports, definitions, diagnostics, and runtime error behavior.

**Migration:** expose an incremental wrapper around the production front end and
VM. Keep session state separate from compiler-global state, and use the same
module identity and artifact rules as project builds.

**Quantitative gate:** every transcript case registered in the M0A inventory is
deterministic; definitions persist exactly when specified; every accepted form
has the same semantic result as a corresponding file; and runtime errors leave
the session in a documented, testable state.

**Old-path deletion condition:** remove any one-off REPL parser/evaluator once
all transcript cases use the production semantic and runtime services.

### Milestone 5D: Source debugger and tracing

**Dependency:** M4B source/debug metadata and stable runtime stack behavior.

**Deliverable:** add source-level stack inspection, function/line tracing,
breakpoints or an equivalent stepping protocol, and runtime-value inspection.

**Migration:** build on emitted debug metadata and VM events; do not create a
second source-location mapping scheme in the debugger.

**Quantitative gate:** every frame in the named debugger corpus registered in
the M0A inventory maps to a source range; stepping through the corpus produces
deterministic event order; and inspected values agree with VM output for all
supported runtime kinds.

**Old-path deletion condition:** remove temporary debug-location tables when
all debugger events use the M4B metadata contract and no runtime frame requires
fallback source guessing.

## Conditional research track

These are valid directions, but they are not the active queue:

- recursive structs and richer nominal/structural type relationships;
- protocols, traits, dynamic dispatch, inheritance, or overloading;
- strings as iterable values and a general iterator abstraction;
- garbage collection, task scheduling, async execution, and JIT compilation;
- package registries, dependency resolution, and large standard-library design.

Each item needs a separate design decision covering user value, semantics,
module/interface impact, diagnostics, runtime representation, quantitative
workload, and verification. It should enter the active roadmap only if the
shared foundations above make the investment tractable and a concrete use case
warrants it.

## Superseded roadmap disposition

The 2026-07-22 re-baseline summarizes major open areas from the immediately
preceding feature-oriented roadmap, preserved at Git revision
`5404796:docs/roadmap.md`. This table records scheduling intent rather than every
low-level implementation candidate; Git history remains the detailed source for
the superseded wording.

| Earlier item | Disposition in this roadmap |
| --- | --- |
| Verification orchestration, robustness, property/fuzz testing, and measurement | M0A-M0D. |
| Shared front-end ownership and duplicated checker/lowering work | M1; a unified assignment AST, visitor framework, or similar code-health refactor remains an implementation choice unless it proves a milestone outcome. |
| Flow/nullable consistency, parser/type recovery, recursive-type policy, and language-wide semantic cleanup | M2A/M2B, one explicit behavior-change or recovery slice at a time. |
| Module interfaces, linker inputs, separate compilation, and incremental builds | M3A/M3B. |
| `.cdbc` evolution, validation, runtime/debug contracts, and measured IR/backend optimization | M0.5B and M4A/M4B; optimization additionally requires the M0D baseline. |
| Formatter, LSP, REPL, and debugger | Independent M5A-M5D schedules. |
| Further isolated builtins, generic container conveniences, or legacy `len` lowering cleanup | Not an active queue; admissible only as the smallest proof of a foundation milestone. |
| Comments/doc comments and formatter prerequisites | Comment/trivia preservation and full ranges are promoted to M0.5A, M1A1/M1A2, and M5A; doc-comment syntax still requires a separate language decision. |
| Broader loop, post-branch, field, and index nullable narrowing | Intentionally reopened for an M2 decision; current documented behavior remains unchanged until then. |
| Direct CLI multi-file compilation | Preserved as one ordered entry program in M3A; changing it is outside the import-path migration. |
| Recursive structs, richer object systems, iterators, GC, scheduling, and JIT work | Deferred to the conditional research track and not silently dropped. |
| Already implemented language/API items | Removed from the active queue; their contract lives in `README.md`, the grammar, tests, and artifact documentation. |

## Work admission rules

Before starting a substantial change, its design document must answer:

1. Which roadmap capability and which concrete milestone/slice does it advance?
2. Does it reduce duplicated compiler/VM work or unlock more than one future
   feature area?
3. What are its source, type, diagnostic, IR, bytecode, and runtime contracts?
4. Which inventory revision and case IDs define the baseline, and what target,
   measurement command, recorded output, and exit gate apply?
5. How does the new path coexist with the old path during migration?
6. What exact condition permits deleting the old path?
7. What behavior is intentionally not included?

An isolated builtin, syntax convenience, or VM opcode should not become the
next priority merely because it is easy to implement. It may still be used as
the smallest proof of a broader milestone.

## Near-term execution order

The only immediate slice is M0A: inventory the existing checks and expose one
canonical verification command. After M0A, M0B, M0C, M0D, M0.5A, and M0.5B are
independently deliverable and may proceed in parallel when ownership permits.

M0A is implemented at inventory revision `m0a-2026-07-22-r1` against baseline
commit `0481624`. The checked-in inventory contains 1,563 stable case IDs;
`tests/run_verification.py` is the canonical runner and writes a machine-
readable report, while the legacy CI commands remain active during the
two-run migration gate. The design and observed baseline are recorded in
`docs/verification/m0a-inventory-design.md` and
`docs/verification/m0a-baseline.json`. M0B is the next foundation slice after
this migration evidence is accepted.

M0B is implemented at inventory revision `m0b-2026-07-22-r1` against the M0A
reference commit `6feb009`. The canonical report now records boundary
sequences and first-failure attribution across 13 supported boundaries, and
the five-case token corpus plus mismatch selftests are checked in under
`tests/` and `docs/verification/`. The observed M0B baseline is recorded in
`docs/verification/m0b-boundary-design.md` and
`docs/verification/m0b-baseline.json`; M0C, M0D, M0.5A, and M0.5B are the next
independently deliverable foundation slices.

M0C is implemented at corpus revision `m0c-2026-07-22-r1` against the M0B
reference commit `30ae329`. The bounded deterministic corpus has 88 cases,
including lexer/parser seeds, the existing parse-error family, and `.cdbc`
mutations; the canonical inventory now reports 1,658 cases. The harness,
minimizer selftest, and observed baseline are recorded in
`docs/verification/m0c-malformed-design.md` and
`docs/verification/m0c-baseline.json`. M0D and M0.5A/M0.5B are the remaining
independently deliverable foundation slices.

The hard dependency gates are:

```text
M0B boundary reference + M0.5A resolved semantic decisions
  -> M1A1 snapshot identities/source ranges
  -> M1B declaration collection/name resolution
  -> M1C typed expressions/assignment targets
  -> M1D control flow/functions/closures
  -> M1E1 type core/generics
  -> M1E2 collections/aggregates
  -> M1E3 patterns/coverage inputs
  -> M1F semantic-front-end cutover
  -> M2A coherent semantics and M3A graph/interfaces

M0C malformed/recovery corpus -> M2B parser recovery
M1F semantic cutover ----------> M2B type/semantic recovery
M1A1 source ranges ------------> M1A2 lossless token/trivia view

M0B boundary comparison + M0C malformed corpus
  + resolved M0.5A runtime-ABI decisions + M0.5B cdbc 0.1 audit
  -> M4A artifact validation/compatibility

M3A + internal-module-object decision -> M3B
M3A + M4A + per-module-.cdbc decision -> M3B
M1A1 source identities + M3A module identities + M4A -> M4B debug/runtime metadata
M0D verification matrix -> M1F final cutover
M0D measurement baseline -> any performance gate or optimization claim
```

These are dependency edges, not a requirement to finish M2, M3, and M4 in one
serial order. M3B records its artifact choice before accepting the conditional
M4A edge.

Independent tool schedules start after their actual prerequisites:

```text
M1A2 lossless syntax/trivia --------------------------> M5A formatter
M1 semantic services + M3A graph/interfaces ---------> M5B LSP
M3A session boundary + M3B where needed + M4A/M4B ---> M5C REPL
M4B source/debug metadata ----------------------------> M5D debugger
```

Keeping these schedules separate prevents one delayed tool from blocking the
others. The immediate project is therefore M0A only; its first checked-in result
is the versioned case inventory, canonical command, and legacy-comparison report.

## Metrics dashboard

Track these measures in each milestone report instead of counting APIs:

- **Correctness:** full-suite pass rate, unexplained cross-backend mismatches,
  malformed-input crashes, and deterministic-output failures;
- **Migration:** percentage of named case IDs on the new path, shadow-mode
  mismatch count, production legacy-path invocation count, and deleted duplicate
  helpers;
- **Semantic architecture:** number of independent type/name/lowering decision
  sites, snapshot-ID/range coverage, and percentage of backend operations
  carrying resolved semantic data;
- **Modules:** unchanged-dependency cache hit rate, rebuild scope after a leaf
  implementation/interface change, graph diagnostic stability, interface-
  consumption coverage, and dependency-body fallback count;
- **Artifacts/runtime:** supported-version/capability coverage, invalid-artifact
  rejection rate, source-mapped frame coverage, artifact size, compile time, and
  named runtime workload performance against the M0D baseline;
- **Tools:** formatter lossless/comment preservation and idempotence, CLI/LSP
  diagnostic equivalence, REPL transcript determinism, and debugger source-frame
  coverage.

The roadmap is successful when the compiler can grow in coherent vertical
slices without repeatedly redesigning its semantic model, module boundary, or
backend contract, and when every old implementation path has a visible reason
to remain or a verified condition for removal.
