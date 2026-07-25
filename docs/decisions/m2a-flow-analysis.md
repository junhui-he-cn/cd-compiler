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
programs. Field/index mutation, loop and post-branch flow, aliasing, calls, and
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
