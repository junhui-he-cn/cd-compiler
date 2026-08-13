# Grammar simplification plan

Audited 2026-08-13 at `master` commit `972658d1`. The language front end
carries several overlapping or asymmetric syntax features. This plan lists
them with impact and implementation order. Removing a shipped feature changes
the compatibility surface, so each item is delivered as its own verified slice
with golden/README/grammar/decision-record updates.

## Implementation queue

1. **Remove `match` expressions, keep `match` statements.**
   Deletes the parallel `MatchExpr` path in the parser, AST, TypeChecker,
   DeclarationIndex coverage records, and IR lowering. Also resolves the
   statement-arm versus expression-arm separator inconsistency. Status:
   complete (`48c4b354`).
2. **Remove struct ordering operators (`operator <`, `<=`, `>`, `>=`).**
   Removes `OperatorSignature`, operator metadata in interfaces and `cdi 0.1`
   sidecars, re-export/namespace forwarding, and the hardcoded `Ord`
   capability witness. Status: complete.
3. **Remove named enum payload fields and reordered pattern arguments.**
   Payloads become purely positional in declarations, constructors, and
   patterns. Status: complete.
4. **Reassess the capability system (`Eq`/`Ord`/`Hash` constraints).**
   Decision gate: delete entirely, or keep only the parts with real use.
5. **Reassess tooling surface.**
   Decision gate: formatter/lossless-source/`--tokens` merge are product
   decisions, not grammar work.
6. **Reduce nullable flow analysis to a core subset.**
   Highest code win, highest semantic risk; requires a separate semantic
   decision before any implementation.

C-style `for` is kept as a supported loop form and is not part of this plan.

## Grammar inconsistencies tracked alongside

- Match arm terminators (resolved by item 1).
- Named enum payloads versus positional constructors (item 3).
- Stale `operator` "not exported" note in `language-grammar.ebnf` (item 2).
- Stale `assignmentTarget` EBNF rule mentioning `call`.
- Optional versus always-used parentheses around `if`/`while` conditions.
- `nil`/`false` falsey but `0`/`""` truthy; then-only narrowing asymmetry.
- No string escape sequences.
- Compound assignment numeric-only while plain assignment is polymorphic.
- Re-export cannot rename names; no `export *`.
- Struct literal versus enum-variant constructor asymmetry.
- Operator declaration special cases (one same-typed parameter, forced
  `: bool`, implicit `this` receiver).

Items 1-3 are complete. Item 4 is a decision gate, not an implementation
slice; items 5-6 are not in the default queue without a product decision.
