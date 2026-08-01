# Issue #16: Stable node references and recursive mutable structures Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let library code express recursive mutable nodes with stable aliases,
documented mutation/lifetime behavior, and identical C++/Rust equality and
debug output.

**Architecture:** Reuse the existing nominal struct representation as the
reference type. Named structs already carry identity and shared mutable fields
at runtime, so the first implementation removes the recursive-declaration
type-checking prohibition, uses `optional<Node<T>>` for empty links, and keeps
the existing `Struct`, `Field`, and `AssignField` bytecode operations. Cycle
safe formatting is added around the existing identity values; no new `ref<T>`
syntax, opcode, or `cdbc` version is needed.

**Tech Stack:** C++17 frontend/runtime utilities, CMake/CTest, Python golden
and artifact runners, Rust `Rc<RefCell<...>>` VM values, `cdbc 0.1`, and `cdi`
module-interface sidecars.

---

## Scope and semantic contract

The branch starts from `origin/master` at `189846b4` and is named
`feat/issue-16-recursive-node-references`.

The implementation must freeze these rules before adding library code:

1. A named struct value is a stable strong handle. Assignment, parameter
   passing, closure capture, and field storage copy the handle; they do not
   copy the object. A field mutation is visible through every alias.
2. A struct field may refer to the struct currently being checked, to another
   struct in the same recursive declaration group, or to an array/function/
   `optional<T>` containing such a type. The type is nominal, so this does not
   create an infinitely nested `TypeInfo` object.
3. `nil` is the empty-link value. Replacing `parent.next` with `nil` removes
   that graph edge but never invalidates an existing handle to the former
   child. C++ `shared_ptr` and Rust `Rc` keep a node alive while a strong
   handle or reachable strong cycle remains; cyclic graphs are retained until
   VM teardown because cycle collection is outside this slice.
4. Struct equality and hashing remain identity-based. Recursive values are not
   compared or hashed by traversing fields.
5. Acyclic `print`/`str` output remains byte-for-byte compatible. A reference
   encountered again on the active formatting path prints the deterministic
   marker `<cycle>`; no host pointer or allocator address is printed.
6. Borrow checking, weak references, finalizers, cycle collection, explicit
   `ref<T>` syntax, iterator invalidation, and automatic node deletion are
   deferred. The only invalidation operation in this slice is explicit field
   replacement by the program.

## Task 1: Freeze the design and add failing coverage

**Files:**

- Create: `docs/superpowers/specs/2026-08-01-recursive-node-references-design.md`
- Create: `docs/decisions/m8-language-recursive-node-references-001.md`
- Create: `docs/decisions/m8-language-recursive-node-references-001.json`
- Create: `tests/golden/recursive_node_references/input.cd`
- Create: `tests/golden/recursive_node_type_shapes/input.cd`
- Create: `tests/golden/type_errors/recursive_node_wrong_field_type.cd`

- [ ] Record the six rules above, the absence of parser/lexer changes, the
  existing C++ `StructValue`/Rust `StructValue` representations, and the
  explicit strong-cycle lifetime boundary in the design and machine-readable
  decision.
- [ ] Add the runtime fixture below before changing the checker. It must
  exercise an alias after link deletion, mutation through an alias, a two-node
  cycle, identity equality, and deterministic formatting:

```cd
struct Node<T> {
  value: T,
  next: optional<Node<T>>
}

let tail = Node<number> { value: 2, next: nil };
let head = Node<number> { value: 1, next: tail };
let alias = tail;
head.next = nil;
alias.value = 20;
print head.next;
print tail.value;

let first = Node<number> { value: 3, next: nil };
let second = Node<number> { value: 4, next: first };
first.next = second;
print first;
print first == first;
print first == second;
```

Expected VM output after implementation:

```text
nil
20
{value: 3, next: {value: 4, next: <cycle>}}
true
false
```

- [ ] Add a compile-only type-shape fixture containing direct self-reference,
  `optional<Node>`, recursive arrays, recursive function signatures, generic
  self-reference, and mutual `A`/`B` references. It must have `ast.out` after
  the implementation and must not rely on runtime construction of an
  impossible non-null recursive value.
- [ ] Add a negative fixture proving that recursion does not disable ordinary
  field checking: `struct Node { next: optional<Node> }` followed by
  `Node { next: 1 }` must remain a type error.
- [ ] Run the new fixture subset against the unchanged compiler and record the
  expected initial failures before implementation:

```sh
cmake -S . -B build
cmake --build build
python3 tests/run_golden_tests.py ./build/compiler_design --case recursive_node
```

Expected result: the positive recursive fixtures fail with the current
`recursive struct field` diagnostic, while the wrong-field-type fixture keeps
its existing type-error behavior.

## Task 2: Admit recursive nominal struct types in the checker

**Files:**

- Modify: `src/TypeChecker.cpp:resolveSimpleStructFieldAnnotation`
- Modify: `tests/golden/type_errors/recursive_struct_array.cd`
- Modify: `tests/golden/type_errors/recursive_struct_direct.cd`
- Modify: `tests/golden/type_errors/recursive_struct_function_parameter.cd`
- Modify: `tests/golden/type_errors/recursive_struct_function_return.cd`
- Modify: `tests/golden/type_errors/recursive_struct_indirect.cd`
- Modify: `tests/golden/type_errors/recursive_struct_nullable.cd`
- Modify: `tests/golden/type_errors/generic_struct_recursive.cd`
- Create or refresh: `tests/golden/recursive_node_references/ast.out`
- Create or refresh: `tests/golden/recursive_node_references/ir.out`
- Create or refresh: `tests/golden/recursive_node_references/bytecode.out`
- Create or refresh: `tests/golden/recursive_node_references/run.out`
- Create or refresh: `tests/golden/recursive_node_type_shapes/ast.out`
- Create: `tests/golden/type_errors/recursive_node_wrong_field_type.err`
- Create: `tests/golden/type_errors/recursive_node_wrong_field_type.exit`

- [ ] Change only the `StructCheckState::Checking` branch so it resolves the
  already predeclared nominal `StructTypeDecl` and validates generic arity and
  bounds, instead of throwing the recursive-field diagnostic. Keep the
  `Declared` path's dependency checking and the `Checked` path unchanged.
- [ ] Confirm that `TypeInfo` remains finite: recursive fields contain a
  `namedStructType` and optional/array/function wrappers, never an embedded
  struct field table. Do not add recursive ownership to `TypeInfo`.
- [ ] Move the seven former recursive-struct rejection cases into the positive
  recursive corpus or replace them with the positive type-shape fixture; do
  not leave a stale type-error golden that asserts behavior issue #16 removes.
  Retain the new wrong-field-type fixture as the negative boundary.
- [ ] Refresh only the affected goldens after the checker passes:

```sh
python3 tests/run_golden_tests.py ./build/compiler_design --update \
  --case recursive_node
```

- [ ] Verify that recursive generic field types, mutual recursion, ordinary
  field assignment, nullable narrowing after `if (node != nil)`, and existing
  non-recursive forward references all pass before touching runtime formatting.

## Task 3: Make C++ and Rust formatting cycle-safe

**Files:**

- Modify: `src/Value.cpp:valueToString`
- Modify: `tests/value_tests.cpp`
- Modify: `vm-rs/src/value.rs:Display for Value`
- Modify: `vm-rs/src/value.rs` unit tests

- [ ] Add a private C++ formatting context containing the active identities of
  arrays, maps, and structs. Enter an identity before descending into its
  fields/elements, emit `<cycle>` when that identity is already active, and
  remove it on return so repeated non-cyclic aliases still print normally.
- [ ] Add the equivalent Rust formatter helper using a `HashSet` of
  `(reference-kind, identity)` and keep `Display` as the public entry point.
  Avoid recursively calling `Display` from inside the helper, otherwise each
  nested value would lose the active-path set.
- [ ] Preserve current primitive, array, map, range, struct, and enum output
  for acyclic values. The C++ and Rust cycle fixture must assert exactly
  `{next: <cycle>}` for a one-field self-cycle and the nested output shown in
  Task 1 for the two-node cycle.
- [ ] Extend `tests/value_tests.cpp` with a manually constructed C++
  self-referential `StructValue`, identity-equality assertions, and the exact
  cycle marker. Extend Rust value tests with an `Rc<RefCell<Vec<Value>>>`
  self-reference and the same assertions.
- [ ] Run the focused native and Rust tests:

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R 'value_tests'
cargo test --manifest-path vm-rs/Cargo.toml value -- --nocapture
```

## Task 4: Prove artifact and module-interface compatibility

**Files:**

- Create: `tests/golden/recursive_node_import/input.cd`
- Create: `tests/golden/recursive_node_import/lib.cd`
- Create or refresh: `tests/golden/recursive_node_import/ast.out`
- Create or refresh: `tests/golden/recursive_node_import/ir.out`
- Create or refresh: `tests/golden/recursive_node_import/bytecode.out`
- Create or refresh: `tests/golden/recursive_node_import/module-interface.out`
- Create or refresh: `tests/golden/recursive_node_import/run.out`
- Create: `tests/bytecode_artifacts/recursive_node_references/input.cd`
- Create: `tests/bytecode_artifacts/recursive_node_references/expected.cdbc`
- Create: `tests/bytecode_artifacts/recursive_node_references/run.out`
- Modify: `tests/module_interface_artifact_tests.cpp`
- Modify: `tests/bytecode_module_artifact_tests.py`
- Modify: `tests/bytecode_module_cache_tests.py`

- [ ] Export a generic recursive struct from `lib.cd`, import it through the
  normal source graph, construct a node in the entry module, mutate it through
  an imported method or field permitted by the existing visibility rules, and
  assert the imported `module-interface` output contains the finite shape
  `optional<Node<T>>`.
- [ ] Add a C++ `.cdi` round-trip assertion for a recursive struct field and
  assert that the decoded field has the same `typeInfoName` and public shape.
  Add a cache invalidation assertion showing that changing the recursive field
  shape changes the public-interface hash.
- [ ] Emit and run a linked `.cdbc` fixture through the Rust VM. Confirm that
  the generated instruction stream still uses `Struct`, `Field`, and
  `AssignField`; no new opcode, artifact section, or `cdbc` version is allowed.
- [ ] Run the focused cross-boundary checks:

```sh
python3 tests/run_golden_tests.py ./build/compiler_design --case recursive_node
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
```

## Task 5: Add a library proof without changing existing APIs

**Files:**

- Create: `library/linked_structures.cd`
- Modify: `library/data_structures.cd`
- Modify: `library/README.md`
- Modify: `library/DATA_STRUCTURES_ROADMAP.md`
- Modify: `library/TODO.md`
- Create: `library/tests/linked_structures_basic/input.cd`
- Create: `library/tests/linked_structures_basic/run.out`

- [ ] Add a generic `LinkedNode<T>`/`LinkedList<T>` implementation using
  `optional<LinkedNode<T>>` links and the existing struct-reference behavior.
  Expose factories and methods for empty construction, front/back insertion,
  front removal, node-handle lookup, node value/next inspection, and a shallow
  value snapshot. Keep implementation fields private and preserve the
  `data_structures.cd` facade exports.
- [ ] Document that a node handle remains valid after unlinking, that aliases
  observe value/link mutation, that empty removal returns `nil`, and that a
  reachable strong cycle retains nodes until VM teardown. Record linear
  traversal complexity and explicitly avoid claiming a cycle collector.
- [ ] Add the separate library fixture runner coverage for an empty list,
  aliases, deletion of a referenced node, and a deliberately cyclic pair. The
  fixture must assert the `<cycle>` output and the post-unlink handle value.
- [ ] Keep the existing array-backed LRU/LFU cache APIs unchanged. Mark
  node-based cache migration as the next library slice now that its handle
  contract is explicit; do not silently change cache key ownership in this
  issue.
- [ ] Run the isolated library checks:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 library/tests/run_tests.py \
  ./build/compiler_design vm-rs --case linked_structures
```

## Task 6: Update contracts and complete verification

**Files:**

- Modify: `README.md`
- Modify: `docs/roadmap.md`
- Modify: `docs/language-grammar.ebnf` only if the final accepted syntax
  differs from the existing `optional<T>` grammar
- Refresh: `tests/verification_inventory.json`

- [ ] Replace the README statement that recursive struct fields are rejected
  with the stable-handle, alias, unlink, lifetime, equality, and `<cycle>`
  rules. Keep the grammar unchanged if no new tokens or productions were
  added.
- [ ] Admit `M8-LANG-REF-001` in the roadmap with its explicit non-goals and
  quantitative gate; remove the item from the conditional research list only
  after all focused tests pass.
- [ ] Refresh the verification inventory explicitly and inspect its generated
  case metadata:

```sh
python3 tests/verification_inventory.py --write
```

- [ ] Rebuild and run the repository gate before claiming completion:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
python3 tests/run_golden_tests.py ./build/compiler_design
python3 tests/run_golden_tests_selftest.py
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
python3 tests/lsp_tests.py ./build/compiler_design
python3 tests/debugger_tests.py ./build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
PYTHONDONTWRITEBYTECODE=1 python3 library/tests/run_tests.py ./build/compiler_design vm-rs
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/verification_inventory.py
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
python3 tests/run_boundary_tests.py ./build/compiler_design
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs --report build/malformed-report.json
git diff --check
rm -rf tests/__pycache__
```

- [ ] Report the exact counts, pre-existing failures if any, branch name, and
  clean-tree state. Commit and push remain separate delivery actions and are
  not part of this plan until explicitly requested.

## Self-review checklist

- The plan changes no parser or bytecode syntax because `optional<T>`, named
  struct construction, field access, and field assignment already exist.
- All issue requirements map to a task: recursive representation (Task 2),
  alias/lifetime/invalidation (Tasks 1 and 5), cycles and debug formatting
  (Task 3), equality (Tasks 1 and 3), C++/Rust parity (Tasks 3 and 4), and
  empty/deleted/alias/cycle tests (Tasks 1, 3, and 5).
- Existing negative field-type checking, private-field visibility, module
  sidecars, and array-backed caches remain explicit compatibility boundaries.
