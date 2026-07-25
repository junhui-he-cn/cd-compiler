# M2A flow-analysis decision: assignment invalidation

The first M2A semantic slice revises `SEM-TYPE-002` from the M0.5A baseline.
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
