# M7-LANG-OPTIONAL-001: canonical optional type syntax

Status: resolved; the implementation slice is complete on
`feat/optional-type-syntax`, while the legacy spelling remains in its
compatibility window.

## Question

How should source code spell a value that may contain `T` or `nil` as the
language grows more generic and compositional type annotations?

## Decision

Make `optional<T>` the canonical source spelling for nullable types. The
parser accepts it recursively wherever a type annotation is accepted, the AST
preserves that spelling, and the type checker maps it to the existing
`StaticType::Nullable` representation. Postfix `T?` remains accepted as a
compatibility spelling during migration. The two spellings have the same
assignment, inference, flow-narrowing, function parameter/return, collection,
struct-field, enum-payload, and pattern semantics.

`optional` is recognized contextually only when followed by `<`; it does not
become a reserved identifier. Missing arguments and missing closing delimiters
remain parser diagnostics. Direct optional function values such as
`optional<fun(number): number>` are rejected because nullable function values
were outside the previous contract. Nullable function parameters and returns,
such as `fun(optional<number>): optional<string>`, remain supported.

## Compatibility and migration

The migration is source-surface-only. IR, bytecode, `.cdbc 0.1`, Rust VM
runtime values, and module-cache identity remain unchanged. New documentation
and fixtures use `optional<T>`, including nested arrays and nullable arrays;
existing `T?` fixtures remain as compatibility coverage. Semantic display names
from `typeInfoName()` continue to use postfix `?` for stable diagnostics and
interface text until a separate output-format decision is made.

The legacy parser path may be removed only after repository-owned examples and
active fixtures no longer need it, an audited compatibility corpus has no
required `T?` source outside an explicit allowlist, and a follow-up decision
defines deprecation diagnostics and the release timeline. This slice does not
emit warnings or remove the old path.

## Quantitative gate

The focused corpus covers canonical AST/IR/bytecode output and runtime
execution, a missing `optional<>` argument parse diagnostic, and rejection of
optional function values in both bindings and struct fields. The verification
inventory records these cases by their stable fixture-based IDs, including
`golden.success.optional_type_syntax.*`,
`golden.parse_errors.optional_missing_argument`,
`golden.type_errors.optional_function_value`,
`golden.type_errors.optional_function_field`, and
`rust_vm.golden.optional_type_syntax.*`.

## Out of scope

- a runtime `Option`/wrapper value or a new bytecode opcode;
- removing postfix `T?` in this release;
- warnings, automated source rewriting, or deprecation diagnostics;
- nullable function values; and
- changing semantic display or module-interface text from `T?` to
  `optional<T>`.

## Verification

The focused and repository-wide commands are the canonical commands in
`docs/roadmap.md`. The implementation must rebuild before running them, refresh
`tests/verification_inventory.json` after fixture discovery changes, remove
`tests/__pycache__/`, and pass `git diff --check` before delivery.

The feature-branch gate recorded on 2026-07-29 is:

- verification inventory: 1,879/1,879;
- boundary token tests: 5/5;
- malformed corpus: 104/104;
- CTest: 33/33;
- golden tests: 828/828;
- golden-runner selftests: 24/24;
- bytecode artifacts: 118/118;
- module cache: 11/11;
- Rust VM goldens: 778/778;
- Cargo tests: 48/48; and
- LSP, debugger, module artifact, JSON validation, and `git diff --check`
  passed.
