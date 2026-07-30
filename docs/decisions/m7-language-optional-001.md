# M7-LANG-OPTIONAL-001: canonical optional type syntax

Status: resolved; the implementation slice is complete and the legacy
postfix spelling has been removed.

## Question

How should source code spell a value that may contain `T` or `nil` as the
language grows more generic and compositional type annotations?

## Decision

Make `optional<T>` the source spelling for nullable types. The parser accepts it
recursively wherever a type annotation is accepted, the AST preserves that
spelling, and the type checker maps it to the existing `StaticType::Nullable`
representation. Postfix `T?` is rejected with a parser diagnostic. Optional
function values, including `optional<fun(number): number>`, have the same
assignment, inference, flow-narrowing, function parameter/return, collection,
struct-field, enum-payload, and pattern semantics as other optional types.

`optional` is recognized contextually only when followed by `<`; it does not
become a reserved identifier. Missing arguments, missing closing delimiters,
and postfix `?` remain parser diagnostics. Nullable function parameters and
returns, such as `fun(optional<number>): optional<string>`, remain supported.

## Compatibility and migration

The migration is source-surface-only. IR, bytecode, `.cdbc 0.1`, Rust VM
runtime values, and module-cache identity remain unchanged. Documentation and
active fixtures use `optional<T>`, including nested arrays, nullable arrays,
and optional function values. `typeInfoName()` and module-interface text use
the canonical `optional<T>` spelling. The dedicated postfix fixture remains
only to assert the removal diagnostic; no compatibility parser path or
deprecation warning is provided.

## Quantitative gate

The focused corpus covers canonical AST/IR/bytecode output and runtime
execution, optional function values, missing `optional<>` arguments, and
postfix `?` removal diagnostics. The verification inventory records these cases
by their stable fixture-based IDs, including
`golden.success.optional_type_syntax.*`,
`golden.success.optional_function_values.*`,
`golden.parse_errors.optional_missing_argument`,
`golden.parse_errors.nullable_double_question`, and
`rust_vm.golden.optional_type_syntax.*`.

## Out of scope

- a runtime `Option`/wrapper value or a new bytecode opcode;
- warnings, automated source rewriting, or deprecation diagnostics.

## Verification

The focused and repository-wide commands are the canonical commands in
`docs/roadmap.md`. The implementation must rebuild before running them, refresh
`tests/verification_inventory.json` after fixture discovery changes, remove
`tests/__pycache__/`, and pass `git diff --check` before delivery.

The integration gate recorded on 2026-07-30 is:

- verification inventory: 1,888/1,888;
- boundary token tests: 5/5;
- malformed corpus: 108/108;
- CTest: 34/34;
- golden tests: 828/828;
- golden-runner selftests: 24/24;
- bytecode artifacts: 118/118;
- module cache: 11/11;
- Rust VM goldens: 782/782;
- Cargo tests: 79/79;
- independent library fixtures: 83/83; and
- LSP, debugger, module artifact, JSON validation, and `git diff --check`
  passed.
