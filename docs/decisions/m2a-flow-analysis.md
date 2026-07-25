# M2A flow-analysis decisions

The M2A flow slices revise `SEM-TYPE-002` from the M0.5A baseline.
The M1 migration preserved the old behavior in which an active direct-variable
nullable narrowing survived an assignment to that same binding. M2A now treats
the assignment as a mutation of the binding cell and invalidates the proof.

## Rule

After a successful `name = expression` or numeric `name += expression` (and its
other compound-assignment forms), `FlowFacts` records an invalidation for the
resolved binding name. The current flow region no longer exposes the previous
non-null type. A later direct check such as `name != nil` or `name == nil` in an
applicable branch can establish a fresh fact.

Invalidation state is propagated to enclosing active flow regions that may
observe a nested mutation; a narrowing introduced only by the nested region is
still discarded when that region finishes, including when semantic checking
throws. This slice does not infer a stronger fact from the assigned RHS; it
deliberately returns to the binding's declared/inferred type.

## Compatibility boundary

This is a type-checking behavior change only. Parser, AST, register IR,
bytecode, artifact text, and Rust VM contracts are unchanged for accepted
programs. Field/index mutation, loop and broader post-branch flow, aliasing, calls, and
closure boundaries remain the existing conservative contract and are admitted
as later M2A slices.

The positive fixture proves that a new nil check still works after mutation; the
negative fixture proves that using the stale non-null proof is rejected.

## Migration and gate

`FlowFacts` stores invalidation state beside narrowings. The
existing branch-fact API remains the only producer of nullable proofs, and
`TypeChecker` records an invalidation only after the assignment has passed its
normal type checks. No duplicate checker or lowering path is introduced.

The M0D inventory revision remains `m0d-2026-07-22-r1`; after this fixture
addition it validates 1662 cases. The focused case IDs are:

- `golden.type_errors.nullable_narrowing_assignment_invalidated`
- `rust_vm.golden.nullable_narrowing_assignment_recheck.emit`
- `rust_vm.golden.nullable_narrowing_assignment_recheck.run`

The focused gate is the FlowFacts CTest, the nullable assignment golden subset,
the two Rust VM cases above, and `python3 tests/verification_inventory.py`.
The full canonical verification command remains the release gate.

There is no compatibility implementation to delete in this slice. The old
behavior is replaced only after the shared FlowFacts invalidation record and
the positive/negative mutation fixtures pass; later M2A slices may remove
remaining TypeChecker special cases only under the roadmap deletion condition.

## M2A-FLOW-002: function-boundary isolation

Every function body is checked with an empty active flow-fact environment.
Named functions, struct methods, and anonymous functions all use the shared
`checkFunctionBody` entry point, so a function declared inside `if (x != nil)`
cannot use that definition-site proof. Parameters, local variables, and new
nil checks inside the function continue to work normally.

This is intentionally separate from call-effect analysis. The checker does not
yet invalidate a caller's facts merely because a called function or closure may
mutate a captured binding; that alias/call rule requires a later M2A decision.
The positive fixture accepts a nullable-compatible closure body, while the
negative fixture rejects a body that relies on the enclosing branch.

The decision revision is `m2a-2026-07-25-r2`, based on commit `95227a6`.
The focused inventory cases are:

- `golden.type_errors.nullable_narrowing_function_boundary`
- `rust_vm.golden.nullable_narrowing_function_boundary.emit`
- `rust_vm.golden.nullable_narrowing_function_boundary.run`

Together with the previous assignment-invalidation cases, the M0D inventory
validates 1665 cases. The FlowFacts CTest, focused golden and Rust VM subsets,
`python3 tests/verification_inventory.py`, and the full canonical verification
command are the gates for this revision.

## M2A-FLOW-003: direct captured-call invalidation

When a resolved direct call targets a local function whose capture metadata
names an enclosing binding, the active narrowing for each visible captured
binding is invalidated after the call has passed argument and call-shape
checking. Existing capture metadata intentionally records reads and writes
together, so this rule is conservative: an ordinary non-capturing call keeps
other active narrowings, while a captured call requires a fresh nil check.

This slice does not model indirect function values, native callbacks, global
aliases, or struct-method side effects. Those call-effect boundaries remain
open for a later M2A decision. The decision revision is
`m2a-2026-07-25-r3`, based on commit `c5087ec`.

The positive fixture calls a captured reader and then rechecks the nullable
binding; the negative fixture calls a captured mutator and then uses the stale
proof. The M0D inventory now validates 1668 cases, including:

- `golden.type_errors.nullable_narrowing_captured_call_invalidated`
- `rust_vm.golden.nullable_narrowing_captured_call_recheck.emit`
- `rust_vm.golden.nullable_narrowing_captured_call_recheck.run`

The FlowFacts CTest, focused golden and Rust VM subsets,
`python3 tests/verification_inventory.py`, and the full canonical verification
command are the gates for this revision.

## M2A-FLOW-004: indirect and dynamic-call invalidation

After successful argument and call-shape checking, an indirect or dynamic
function call invalidates every active nullable narrowing. This includes a
function alias, a function-valued binding initialized from an anonymous closure,
and a callee whose target cannot be resolved by `DeclarationIndex`. A resolved
direct `FunctionStmt` call keeps the precise captured-binding behavior from
M2A-FLOW-003, so direct non-capturing calls do not disturb unrelated facts.

The conservative all-fact rule makes the current absence of function-value
capture summaries explicit. Native stdlib fast paths, native callbacks, and
struct-method effects remain outside this slice. The decision revision is
`m2a-2026-07-25-r4`, based on commit `28649c3`.

The positive fixture re-checks the nullable binding after both a named-function
alias call and an anonymous function-valued call. The negative fixture uses the
stale proof after an alias call and is rejected. The M0D inventory now validates
1671 cases, including:

- `golden.type_errors.nullable_narrowing_indirect_call_invalidated`
- `rust_vm.golden.nullable_narrowing_indirect_call_recheck.emit`
- `rust_vm.golden.nullable_narrowing_indirect_call_recheck.run`

The FlowFacts CTest, focused golden and Rust VM subsets,
`python3 tests/verification_inventory.py`, and the full canonical verification
command are the gates for this revision.

## M2A-FLOW-005: native callback-call invalidation

After successful callback checking, an unshadowed native `map`, `filter`,
`flatMap`, `any`, `all`, `count`, `find`, `findIndex`, or `reduce` call
invalidates every active nullable narrowing. The rule applies to both
function-style helpers and their reserved member-call sugar. Native helpers
without callbacks keep their existing flow behavior.

Namespace calls, enum variant constructors, and struct methods are excluded by
their declaration metadata, so a member spelling alone does not turn an
ordinary function or method into a native callback effect. The decision
revision is `m2a-2026-07-25-r5`, based on commit `120c4a2`.

The positive fixture exercises function-style `map` and member-style `filter`,
rechecking the nullable binding after each call. The negative fixture uses the
stale proof after member-style `map` and is rejected. The M0D inventory now
validates 1674 cases, including:

- `golden.type_errors.nullable_narrowing_native_callback_invalidated`
- `rust_vm.golden.nullable_narrowing_native_callback_recheck.emit`
- `rust_vm.golden.nullable_narrowing_native_callback_recheck.run`

The FlowFacts CTest, focused golden and Rust VM subsets,
`python3 tests/verification_inventory.py`, and the full canonical verification
command are the gates for this revision.

## M2A-FLOW-006: struct-method call invalidation

After successful argument and call-shape checking, every resolved struct-method
call invalidates all active nullable narrowing. This is deliberately
conservative: methods have an implicit receiver, may mutate receiver fields,
and may observe or mutate top-level state that is not represented as a captured
local binding. The rule does not add field/index narrowing.

The implementation reuses `DeclarationIndex`'s `StructMethod` call target and
applies the shared all-fact invalidation only after method checking succeeds.
Unresolved member calls and future dynamic dispatch remain outside this slice.
The decision revision is `m2a-2026-07-25-r6`, based on commit `63148d0`.

The positive fixture calls a method that reads a top-level nullable binding and
then rechecks it. The negative fixture uses the stale proof after the method
call and is rejected. The M0D inventory now validates 1677 cases, including:

- `golden.type_errors.nullable_narrowing_struct_method_invalidated`
- `rust_vm.golden.nullable_narrowing_struct_method_recheck.emit`
- `rust_vm.golden.nullable_narrowing_struct_method_recheck.run`

The FlowFacts CTest, focused golden and Rust VM subsets,
`python3 tests/verification_inventory.py`, and the full canonical verification
command are the gates for this revision.

## M2A-FLOW-007: returning-guard post-branch narrowing

For an `if` without an `else` whose `then` statement is guaranteed not to fall
through through a return, returning block, or returning exhaustive match, the
condition's `else` narrowings become active after the `if`. The checker snapshots
the active facts before the terminating branch, checks that branch with its
normal facts, restores the snapshot, and then installs only the false-branch
facts for the live continuation. Mutations on the dead path therefore do not
contaminate the path after the guard.

Explicit-`else` branches, loops, fields, and indexes remain conservative. The
decision revision is `m2a-2026-07-25-r7`, based on commit `d412116`.

The positive fixture accepts `if (value == nil) { return; }` followed by a
non-null use and also covers a mutation in a returning nested branch. The
negative fixture keeps a non-terminating nil branch and rejects the stale
nullable use. The M0D inventory now validates 1680 cases, including:

- `golden.type_errors.nullable_narrowing_post_branch_unsupported`
- `rust_vm.golden.nullable_narrowing_post_branch_recheck.emit`
- `rust_vm.golden.nullable_narrowing_post_branch_recheck.run`

The FlowFacts CTest, focused golden and Rust VM subsets,
`python3 tests/verification_inventory.py`, and the full canonical verification
command are the gates for this revision.

## M2A-FLOW-008: explicit-else returning-arm narrowing

For an `if` with an explicit `else`, if exactly one arm is guaranteed not to
fall through through a return, returning block, or returning exhaustive match,
the other arm's final nullable flow facts become active after the `if`. Both
arms are checked from the same pre-branch snapshot plus their condition facts
in isolated flow states. The final state of the live arm is installed as the
continuation state, so invalidations in either checked arm are not accidentally
carried across the branch boundary and the live arm's invalidations remain
visible.

When both explicit arms may fall through, the existing conservative behavior is
unchanged. Loops, fields, indexes, and broader branch joins remain outside this
slice. The decision revision is `m2a-2026-07-25-r8`, based on commit `05962f6`.

The positive fixture covers a returning `then` arm whose mutation must not
invalidate the live `else` arm's narrowing. The negative fixture keeps both
arms non-terminating and confirms that a general explicit-`else` join is not
implicitly admitted. The M0D inventory now validates 1683 cases, including:

- `golden.type_errors.nullable_narrowing_explicit_else_unsupported`
- `rust_vm.golden.nullable_narrowing_explicit_else_recheck.emit`
- `rust_vm.golden.nullable_narrowing_explicit_else_recheck.run`

The FlowFacts CTest, focused golden and Rust VM subsets,
`python3 tests/verification_inventory.py`, and the full canonical verification
command are the gates for this revision.

## M2A-FLOW-009: while-body narrowing

For a `while` statement, the condition's true-branch nullable facts are active
while checking the loop body. The facts are scoped to that body and are
discarded at the loop boundary, so the condition does not create a post-loop
non-null proof. Body assignments and calls still use the existing invalidation
rules while the body facts are active.

C-style `for`, `for-in`, loop exits, and broader loop joins remain outside this
slice. The decision revision is `m2a-2026-07-25-r9`, based on commit `a8a9943`.

The positive fixture uses a nullable value in a `while (value != nil)` body and
mutates it before the next condition check. The negative fixture confirms that
the body proof is not retained after a loop that may exit. The M0D inventory
now validates 1686 cases, including:

- `golden.type_errors.nullable_narrowing_while_post_loop_unsupported`
- `rust_vm.golden.nullable_narrowing_while_body_recheck.emit`
- `rust_vm.golden.nullable_narrowing_while_body_recheck.run`

The FlowFacts CTest, focused golden and Rust VM subsets,
`python3 tests/verification_inventory.py`, and the full canonical verification
command are the gates for this revision.
