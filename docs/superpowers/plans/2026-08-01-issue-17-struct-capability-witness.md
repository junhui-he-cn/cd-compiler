# Issue #17 Struct Capability Witness Implementation Plan

> This implementation plan uses checkbox (`- [ ]`) syntax for task tracking.

**Goal:** Allow a named struct with all four ordering operators to satisfy generic `T: Ord` and therefore `T: Eq`, with static validation and identical generic comparison behavior in linked and independent Rust VM artifacts.

**Architecture:** The existing nominal struct/operator metadata remains the single witness source. A struct has an `Ord` witness only when its defining-module operator table contains `<`, `<=`, `>`, and `>=`; identity equality continues to provide the existing base `Eq` behavior. Generic ordering expressions keep the existing comparison IR/bytecode operations, while operator methods on complete witness structs publish a deterministic private runtime binding so the Rust VM can dispatch erased generic operands without adding a user-visible capability dictionary or a bytecode version change.

**Tech Stack:** C++17 TypeChecker/IR compiler, existing `cdbc 0.1` text artifacts and module linker, Rust VM `Value::Struct`/shared environments, Python golden/artifact/cache runners, CTest, and the public `.cd` library.

---

### Task 1: Freeze the witness contract and add red fixtures

**Files:**
- Create: `docs/superpowers/specs/2026-08-01-struct-capability-witness-design.md`
- Create: `docs/decisions/m8-language-struct-capability-witness-001.md`
- Create: `docs/decisions/m8-language-struct-capability-witness-001.json`
- Create: `tests/golden/generic_struct_capability_witness/input.cd`
- Create: `tests/golden/import_struct_capability_witness/input.cd`
- Create: `tests/golden/import_struct_capability_witness/lib.cd`
- Create: `tests/golden/type_errors/struct_capability_missing_operator.cd`

- [x] Define the witness as the complete four-symbol set, with `Ord` implying `Eq`, identity `Eq` remaining available for all existing runtime values, and incomplete sets remaining directly callable but not generic `Ord` witnesses.
- [x] State that only the defining module can provide the operator implementation; imported and re-exported public operator metadata is consumed as the owner’s witness, and duplicate/conflicting slots remain rejected by the existing method table.
- [x] Add a local positive fixture whose generic `T: Ord` function executes all four comparisons on a custom struct and whose `T: Eq + Ord` function executes identity equality.
- [x] Add an imported positive fixture with the same witness crossing a source import and a re-export path.
- [x] Add a negative fixture with only three ordering operators and a generic `T: Ord` call; retain the existing direct partial-operator behavior.
- [x] Run the positive fixture with the current compiler and confirm it fails at the existing `type parameter T must satisfy Ord` diagnostic before implementation.

### Task 2: Make capability validation witness-aware

**Files:**
- Modify: `include/TypeChecker.hpp`
- Modify: `src/TypeChecker.cpp`
- Modify: `include/DeclarationIndex.hpp`
- Modify: `src/DeclarationIndex.cpp`

- [x] Add a TypeChecker-local capability predicate that preserves the existing `Eq`, `Hash`, capability-set, and type-parameter rules while recognizing a complete operator table as `Ord` for a named struct.
- [x] Route generic type-argument constraint validation through that predicate so inferred arguments, explicit arguments, generic struct construction, generic methods, callbacks, imports, and re-exports share one witness decision and the existing diagnostic shape.
- [x] Mark generic ordering expressions only as needed for metadata validation; do not change direct named-struct operator resolution or identity equality.
- [x] Keep `moduleInterfaceMismatchCount()` and existing operator interface records authoritative; no snapshot-local IDs or new sidecar fields are serialized.
- [x] Run the local and negative fixtures to confirm static acceptance/rejection before changing runtime lowering.

### Task 3: Preserve a runtime target for erased generic comparisons

**Files:**
- Modify: `include/IRCompiler.hpp`
- Modify: `src/IRCompiler.cpp`
- Modify: `docs/bytecode-text-format.md`
- Modify: `README.md`

- [x] Scan the checked program for structs with all four operator methods and, while lowering each such operator, store the function value under `__capability_ord_<Struct>_<operator-name>` in addition to its existing resolved binding.
- [x] Use the existing `less`, `less_equal`, `greater`, and `greater_equal` IR/bytecode instructions for generic ordering, so O0/O1, artifact verification, and linker remapping retain their current shapes.
- [x] Document the private deterministic runtime target convention and state that it is an implementation detail of erased generic `Ord`, not a user-visible global or trait object.
- [x] Refresh only the affected local/imported IR and bytecode goldens after the static and lowering tests pass.

### Task 4: Dispatch struct operands in the Rust VM

**Files:**
- Modify: `vm-rs/src/vm.rs`
- Modify: `vm-rs/src/value.rs` tests when focused coverage needs a helper
- Modify: `vm-rs/src/format.rs` only if parser/verifier coverage exposes a missing comparison shape
- Add or modify: `vm-rs` focused VM tests

- [x] Extend the four existing comparison paths to accept two same-named `Struct` values, locate the deterministic private target in the shared global environment, call it with the two operands, and require a boolean result.
- [x] Preserve number/string scalar ordering and the existing unsupported-value diagnostics; missing or malformed witness targets must be stable runtime errors rather than panics.
- [x] Pass the current debug call site through the ordinary function call so runtime failures retain the established stack/location behavior.
- [x] Verify direct calls, generic calls, source imports, re-exports, and independently linked module products execute identically.

### Task 5: Synchronize library/docs/inventory and close the prior issues only after evidence

**Files:**
- Modify: `docs/decisions/m8-language-recursive-node-references-001.md`
- Modify: `docs/decisions/m8-language-recursive-node-references-001.json`
- Modify: `docs/roadmap.md`
- Modify: `library/TODO.md`
- Modify: `library/DATA_STRUCTURES_ROADMAP.md`
- Modify: `library/README.md`
- Modify: `tests/verification_inventory.json` via `python3 tests/verification_inventory.py --write`

- [x] Change the #16 decision status from proposed to implemented and record its verified recursive-node gate.
- [x] Replace the documented “custom structs do not satisfy `T: Ord`” boundary with the complete-witness rule while retaining explicit comparators for partial or comparator-specific algorithms.
- [x] Keep this compiler slice isolated because the existing library algorithms retain explicit comparator APIs; document the future library migration boundary.
- [x] Run focused gates, the canonical verification inventory/report/boundary/malformed commands, all golden and Rust VM goldens, module artifact/cache tests, CTest, Cargo tests, and `git diff --check`.
- [x] If #15 and #16 remain green after the complete gate, report that they satisfy their acceptance criteria and handle remote issue state separately from commit/push/PR/merge actions.
