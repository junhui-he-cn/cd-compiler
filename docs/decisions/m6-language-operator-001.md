# M6-LANG-OPERATOR-001: statically dispatched user-defined ordering operators

Status: design resolved; builtin string-ordering, local named-struct operators,
and the public interface/cache shape phase are implemented. Imported operator
dispatch and independent module-product parity remain deferred.

Baseline: `master` at `2592027` (`feat: add VSCode syntax highlighting`).

Issue: [#10](https://github.com/junhui-he-cn/my-compiler/issues/10).

## Question

How should user-defined types declare `<`, `<=`, `>`, and `>=` so that local
calls, imported interfaces, module products, generic APIs, and both compiler
runtimes have one deterministic contract? The first user-visible builtin
extension is lexicographic comparison for `string`.

## Decision

Add a first comparison-operator slice for named structs. An operator is an
instance-like declaration inside the existing top-level `impl` block:

```cd
struct Person { name: string, age: number }

impl Person {
  operator <(other: Person): bool {
    return this.age < other.age;
  }
}
```

The receiver is the left operand and is exposed to the body as the existing
implicit `this` binding. The one explicit parameter is the right operand. The
declaration has exactly one explicit parameter, requires an explicit `: bool`
return annotation, and has no method-level type parameters in this slice.
Receiver type parameters remain available through the existing `impl Box<T>`
syntax:

```cd
struct Box<T: Ord> { value: T }

impl Box<T: Ord> {
  operator <(other: Box<T>): bool {
    return this.value < other.value;
  }
}
```

The receiver parameters and constraints must match the struct declaration
exactly, using the existing `impl` rules. `operator` becomes a reserved
declaration keyword. The declaration grammar is intentionally narrow:

```text
operatorDecl = "operator", orderingOperator, "(", parameter, ")",
               ":", "bool", block ;
orderingOperator = "<" | "<=" | ">" | ">=" ;
```

Operators are not free functions, expressions, fields, or overloadable
top-level names. Enums and primitive types cannot receive user-defined
operator implementations in this slice. A named struct may implement any
subset of the four operators; each symbol is independently callable, and no
operator is derived from another symbol.

## Static dispatch

The checker resolves a binary comparison after checking the left and right
operands in source order:

1. A known `number`/`number` pair uses the existing numeric builtin.
2. A known `string`/`string` pair uses the new builtin string ordering.
3. A known non-null named-struct left operand selects the implementation for
   that struct and that operator. The right operand must be the same nominal
   struct type with invariant, matching generic arguments. The selected body
   is lowered as an ordinary direct function call with the left value as the
   implicit receiver and the right value as its explicit argument.
4. No candidate, a mixed builtin pair, a mismatched struct right operand, or a
   nullable value without prior supported narrowing produces a type diagnostic
   at the operator token. An unknown operand never triggers a runtime search
   for a user-defined implementation; existing permissive unknown handling for
   unrelated builtin operations remains a separate compatibility behavior.

Lookup is based on nominal type identity, not on the spelling of an import
alias. There is no reverse lookup on the right operand, no conversion, no
fallback from one comparison symbol to another, and no candidate ranking.
Because a struct/operator pair has one owner and one slot, an implementation
cannot become ambiguous through imports, namespace aliases, or re-exports.

An implementation must be declared in the module that defines the named
struct. Implementing an imported, re-exported, primitive, or enum type is
rejected; this is the operator form of the existing owner/module boundary and
avoids foreign implementations and orphan-style conflicts. If the struct is
exported, all of its operator metadata follows the existing exported-struct
method visibility rule. A non-exported struct and its operators remain module
private. Re-exported structs forward their operator metadata together with
their existing public method metadata.

Operator bodies are ordinary methods: they may use existing fields, calls,
mutation, control flow, and nested operators, and their side effects and
runtime failures follow ordinary method/call behavior. The language does not
prove ordering laws such as transitivity or antisymmetry.

## Builtin string ordering

`string` ordering compares the sequence of Unicode scalar values
lexicographically. The first unequal scalar decides the result; when one
sequence is a strict prefix of the other, the shorter sequence is smaller.
There is no locale, case folding, grapheme-cluster ordering, or Unicode
normalization. Thus canonically equivalent but differently encoded sequences
are compared as their supplied scalar sequences. Implementations may use the
existing valid UTF-8 representation as an optimization because its byte order
preserves scalar-value order; the observable contract is scalar-value order.

`number` and `string` each provide all four ordering symbols. `bool`, `nil`,
arrays, maps, ranges, enums, and functions do not gain ordering from this
slice. Nil is never implicitly ordered before or after a non-nil value.

`==` and `!=` retain the current equality behavior, including struct identity
equality and enum structural equality. User-defined equality declarations are
deferred, as are `+`, `-`, `*`, `/`, compound assignment overloads, unary
operator declarations, and logical-operator overloads.

## Generic boundary

This decision deliberately does not turn an operator table into a runtime
capability dictionary. The M6 static `Eq`/`Ord` contract remains compile-time
only: `Ord` admits the builtin `number` and `string` types in this slice, while
a user-defined struct with operator declarations is not implicitly admitted as
`T: Ord`.

Consequently, a generic function such as `fun<T: Ord>` continues to use the
existing statically checked builtin path. Generic algorithms that need to
operate on a user-defined struct use the existing explicit comparator contract
`fun(T, T): bool`; a small ordinary function can wrap `left < right` and be
forwarded through local, imported, namespace-qualified, and re-exported
generic calls. This keeps generic function arity, function values, `.cdi`
interfaces, and erased runtime types unchanged.

Connecting user-defined operator sets to `T: Ord` requires a separate decision
for implicit witnesses, monomorphization, or another static specialization
scheme. It must not be introduced as an incidental implementation detail of
this slice.

## Module, interface, and artifact migration

The implementation reuses the existing module and artifact boundaries:

- `ModuleInterfaceStruct` gains a canonical operator list containing the
  symbol, receiver type, right-parameter type, `bool` result, generic receiver
  metadata, and resolved linkage name. Operators sort by symbol within their
  owning struct. The first phase also forwards that list through explicit
  struct re-exports.
- `cdi 0.1` sidecars include that public operator shape in the interface body
  and public-interface hash. A sidecar that predates or omits the operator
  records is invalid for the current shape and follows the existing source
  fallback or strict-rejection policy; it is never silently trusted.
- The module cache invalidates an importer when an exported operator is added,
  removed, or changes its signature/linkage. Private operator implementation
  body changes retain the existing private-change behavior.
- User-defined operators lower to ordinary function calls, so linked and
  independent `.cdbc 0.1` products need no new operator opcode or runtime
  operator table. The owning module product must retain the operator function
  and its resolved call target; the linker validates it through the existing
  function/cross-reference checks.
- The existing comparison bytecode operations are extended only for builtin
  string ordering. The C++ path and Rust VM must produce the same scalar-order
  results and stable runtime errors for unsupported runtime values. The Rust VM
  does not inspect user-defined operator metadata.
- Snapshot-local module, source, declaration, binding, and scope IDs remain
  non-persistent. Operator linkage names are the only cross-module call
  identity exposed to artifacts.

## Diagnostics

The following diagnostic categories are stable contract points and are
anchored at the relevant operator or declaration token:

- duplicate symbol: `duplicate operator \`<\` for struct \`Point\``;
- invalid receiver/right parameter: `operator \`<\` for struct \`Point\` must
  accept exactly one right operand of type \`Point\``;
- invalid return: `operator \`<\` for struct \`Point\` must return bool`;
- foreign implementation: `cannot implement operator \`<\` for imported
  struct \`lib.Point\``;
- missing use-site implementation: `struct \`Point\` has no implementation
  for operator \`<\``;
- incompatible right operand: `operator \`<\` for \`Point\` expects right
  operand \`Point\`, got \`number\``; and
- unsupported generic capability: the existing `T: Ord` constraint diagnostic
  identifies the user-defined type as not satisfying `Ord`.

The exact source path/line/caret wrapping continues to follow the existing
diagnostic convention for local and imported modules.

## Admission and verification gate

The local implementation slice is admitted in the roadmap. The remaining
public interface/cache migration still requires a focused inventory and
separate admission. Its corpus must independently cover:

- parser, AST, formatter, and invalid declaration cases for all four symbols;
- local struct operators, missing implementations, wrong right types, wrong
  return types, duplicate symbols, and side-effect/evaluation order;
- ASCII, non-ASCII, emoji, prefix, and canonically equivalent string ordering;
- direct imports, namespace aliases, re-exports, owner-module enforcement,
  stale/malformed `.cdi`, public-interface hash changes, and module-cache
  invalidation;
- generic explicit-comparator wrappers around custom operators, plus negative
  attempts to use a custom struct as builtin `T: Ord` in this slice;
- linked and independent `.cdbc 0.1` products, stable resolved call targets,
  C++ execution, Rust VM execution, and runtime parity; and
- the canonical inventory, focused CTest/golden suites, module-interface and
  module-cache tests, artifact tests, Rust VM/Cargo tests, and `git diff
  --check`.

The old direct builtin comparison path may be deleted or replaced only after
the focused corpus proves numeric behavior, new string behavior, direct-call
operator behavior, cache fallback/strictness, and C++/Rust parity. No source
fallback, interface validation, or ordinary method-call compatibility path is
removed by this decision.

## Explicitly deferred

- user-defined `==`/`!=`, arithmetic, unary, logical, and compound-assignment
  operators;
- operator implementations for enums and primitive types;
- dynamic dispatch, trait objects, implicit capability dictionaries, hidden
  comparator parameters, and generic monomorphization/specialization;
- making user-defined operator sets satisfy `Eq`/`Ord` automatically;
- operator values or taking an operator implementation as a first-class
  function without an explicit wrapper; and
- ordering laws, locale-aware collation, Unicode normalization, and custom
  iterator/container protocols.
