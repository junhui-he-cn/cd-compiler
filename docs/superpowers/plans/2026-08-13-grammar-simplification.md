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
   Decision recorded (`m9-language-capability-001.md`): keep the constraint
   system; the struct `Ord` witness was already removed by item 2. Status:
   complete.
5. **Reduce nullable flow analysis to a core subset.**
   Redesign recorded
   (`2026-08-13-explicit-optional-unwrapping-design.md`): replace automatic
   narrowing with explicit `if let`/`while let`, `?`, and `??` unwrapping in
   the Rust style, then delete the flow analysis. Status: complete.

C-style `for` is kept as a supported loop form and is not part of this plan.
The tooling surface (formatter, lossless source, `--tokens`) is kept as
shipped and is not part of this plan.

## Grammar inconsistencies tracked alongside

- Match arm terminators (resolved by item 1).
- Named enum payloads versus positional constructors (item 3).
- Stale `operator` "not exported" note in `language-grammar.ebnf` (item 2).
- Stale `assignmentTarget` EBNF rule mentioning `call` (resolved).
- Optional versus always-used parentheses around `if`/`while` conditions
  (resolved: optional parentheses are kept and documented in
  `USER_MANUAL.md` and `language-grammar.ebnf`).
- `nil`/`false` falsey but `0`/`""` truthy; nil checks are now plain boolean
  tests with no narrowing, so the old then-only asymmetry is gone (resolved as
  a documented language rule).
- No string escape sequences.
- Compound assignment numeric-only while plain assignment is polymorphic.
- Re-export cannot rename names; no `export *`.
- Struct literal versus enum-variant constructor asymmetry.
- Operator declaration special cases (one same-typed parameter, forced
  `: bool`, implicit `this` receiver) (resolved by item 2: operator
  declarations were removed).

Items 1-5 are complete. C-style `for` and the tooling surface are kept as
shipped.
