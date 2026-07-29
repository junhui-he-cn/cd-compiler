# VM-RUNTIME-OWNERSHIP-001：运行时所有权、别名和生命周期

Status: decision recorded on 2026-07-29 against `cae3a44`; no runtime storage
refactor is included in this slice.

## Question and boundary

This record fixes the observable ownership contract that VM-2B must preserve.
The evidence is the current Rust VM in `vm-rs/src/value.rs`,
`vm-rs/src/runtime.rs`, and `vm-rs/src/vm.rs`, the C++ value model in
`include/Value.hpp` and `src/Value.cpp`, and the existing C++ emission/Rust VM
parity corpus. It does not add copy/borrow syntax, a host value API, a garbage
collector, or a new heap representation.

## Decision summary

1. A runtime `Value` is copied by value for primitives and by a shared handle
   for mutable aggregates and closure cells. Assignment, function arguments,
   returns, and native results do not perform an implicit deep copy.
2. Arrays, maps, structs, and functions retain identity across handle copies.
   `==`/`!=` use that identity. Ranges and variants use structural equality;
   variant payloads compare recursively using the same value rules.
3. Aggregate mutation is visible through every alias. Native collection
   constructors return a fresh outer aggregate with shallow element/value
   references unless the operation is explicitly mutating.
4. Closures capture shared cells, not snapshots. Callback execution is
   synchronous and collection helpers iterate a snapshot of the input vector,
   so callback mutation does not extend the current iteration.
5. The current VM is single-threaded and execution-scoped. `run` and `trace`
   consume the VM; current public results contain output, trace text, and
   diagnostics, not live runtime `Value` handles.
6. Runtime errors and resource errors are not transactions. Mutations already
   completed before an error are not rolled back, while `run` does not return
   partial stdout and `trace` retains only its already-emitted textual events.
7. The runtime representation can encode cycles through mutable aggregates and
   closure environments, but the source-level contract for cycles is deferred.
   No future heap abstraction may silently infer cycle collection, acyclic-only
   behavior, or recursive display semantics from `Rc<RefCell<...>>`.

## Value classes

| Value | Current storage/alias rule | Equality and hash rule | Mutation/return rule |
| --- | --- | --- | --- |
| `nil`, number, bool, string | copied as values; Rust strings and C++ strings own their contents | value equality and value hash; `-0` and `0` hash alike, NaN equality remains the host floating-point rule | no shared mutable payload |
| function | `FunctionValue` copies its body index, name, arity, identity, and shared closure environment | identity equality/hash; each `MakeFunction` allocation gets a VM-local identity | callable object is not user-mutable; captured cells are mutable |
| array | identity plus shared mutable element vector (`Rc<RefCell<Vec<Value>>>` in Rust, `shared_ptr` in C++) | identity equality/hash | index assignment and `push`/`pop` mutate the shared vector |
| map | identity plus shared ordered entry vector | identity equality/hash; keys are only `nil`, number, bool, or string | assignment, `remove`, and `clear` mutate entries; duplicate keys keep first position and replace the value |
| struct | identity, optional nominal type name, and shared mutable field vector | identity equality/hash; type name is not part of equality | field assignment mutates every alias |
| range | immediate `start`, `stop`, `step`, and cached length | structural equality/hash; no identity allocation | immutable and not assignable by index |
| enum variant | enum name, variant name, and payload values; payload fields have no mutation opcode | structural recursive equality/hash | constructors create a value; payload aggregate handles remain shallow aliases |

The Rust variant payload vector is owned by the variant while the C++ model
stores the vector behind `shared_ptr`; this representation difference is not
observable because variant fields are read-only at the language/runtime
boundary. The identity counters are VM-local and type-specific; they are not
stable handles that may be persisted across VM instances.

## Alias, copy, and native return rules

- `let alias = value`, assignment, arguments, and returns clone the `Value`
  handle. An array/map/struct mutation through `alias` is visible through
  `value`; a newly constructed aggregate has a new identity even when its
  contents are equal.
- `copy`, `slice`, `concat`, `map`, `filter`, `flatMap`, `keys`, and `values`
  return fresh arrays. Their elements are shallow value copies. `merge`
  returns a fresh map with shallow values; right-hand duplicate values replace
  left-hand values while the original key position is retained.
- `find` returns the matching source element as a shallow value. `reduce`
  threads the accumulator and elements as shallow values; an aggregate
  accumulator is therefore mutated when the callback mutates it.
- `push`, `pop`, `remove`, and `clear` mutate their input aggregate. `push` and
  `clear` return `nil`; `pop` and `remove` return the removed shallow value.
- `range`, `str`, `substr`, `charAt`, `typeOf`, and `hash` return immutable
  values. `str` recursively formats aggregates, so cyclic formatting is not a
  supported contract until the cycle decision is closed.
- Array/map callback helpers clone the input vector before invoking callbacks.
  This fixes length/order for that invocation, while aliases still observe
  callback mutations after the helper returns. Callback frames retain the
  function's closure and their argument cells until the synchronous call ends.

## Closure cells and runtime roots

`Cell` is a shared mutable slot and `Environment` is a shared name-to-cell map.
Function creation copies cell references from the current closure and locals;
it does not copy the values in those cells. This is the basis for mutable
closures, nested recursion, and aliases returned from a factory. Local shadowing
creates a new cell and does not update the shadowed cell.

The current implementation roots runtime objects through:

- `VM.globals`;
- active frame registers, locals, and closure environments;
- synchronous call/native argument vectors, callback frames, and copied
  collection snapshots while an operation is running; and
- temporary values being assembled into an array, map, struct, or variant.

`Program` is borrowed bytecode, not a runtime-object root. Trace events and
`RuntimeError` retain source locations, stack frames, and formatted strings,
not `Value` graphs. On normal or error completion the VM is consumed, so
acyclic runtime objects become unreachable when their reference counts drop.
There is currently no public way to retain a live runtime value after
`run`/`trace` returns.

## Errors, cancellation, and lifetime

There is no source-level exception or rollback mechanism. A callback may mutate
a captured cell or an aliased aggregate before it returns an error; the error
propagates through the native call and stops execution. Resource checks for
allocation growth and output happen before the corresponding growth/write, but
they do not undo earlier successful operations in the same run. `run` suppresses
partial output on failure; `trace` may expose prior textual events and then
reports the final error and call stack.

The reference-counted implementation can represent cycles, for example an
aggregate containing itself or a closure/environment/aggregate cycle. Such a
cycle has no specified source behavior, no cycle detection, and no guaranteed
drop under the current runtime. Recursive `Display` is likewise not a safe
cycle observation. VM-2B must first choose a policy for cycle construction,
formatting, collection, and post-error lifetime; this document intentionally
does not choose one.

The executor is not `Send`/`Sync`: `Rc`/`RefCell` and the C++ shared mutable
model assume one execution thread. `CancellationToken` may be signalled from a
host thread, but it is only a cooperative flag and does not make runtime values
or the VM concurrently usable.

## Decision case matrix

These existing cases are the named VM-2A corpus. The expected observations are
the contract above, not new output formats.

| Case | Existing evidence | Expected observation |
| --- | --- | --- |
| `VM2A-ARRAY-ALIAS-001` | `tests/golden/array_index_assignment`, `tests/golden/native_stdlib_push_pop` | assignment/push through an alias is visible through the original array |
| `VM2A-MAP-ALIAS-001` | `tests/golden/maps`, `map_clear`, `map_remove`, `tests/bytecode_artifacts/map_merge` | map identity, insertion order, replacement, remove/clear, and shallow merge values are stable |
| `VM2A-STRUCT-IDENTITY-001` | `tests/golden/struct_identity_equality`, `struct_methods_mutation`, `import_struct_method_mutation` | same struct alias compares equal and sees field mutation; a fresh equal-shaped struct does not |
| `VM2A-CLOSURE-CELL-001` | `tests/golden/closure_counter`, `closure_shared_cell`, `closure_nested_recursion`, `lambda_mutable_closure` | returned closures share captured cells and recursive/nested calls retain the intended cell |
| `VM2A-SHALLOW-COPY-001` | `tests/golden/array_map`, `array_filter`, `array_reduce`, `map_merge`; Rust tests `native_collection_helpers_query_and_copy_shallowly` and `native_merge_returns_fresh_ordered_map_and_shares_values` | fresh outer collections do not share outer storage, but nested aggregate elements do share handles |
| `VM2A-VARIANT-STRUCTURAL-001` | `tests/golden/adt_pattern_matching`, `generic_enums`, `tests/bytecode_artifacts/nullable_enum_patterns`; Rust `enum_variants_format_and_compare_structurally` | equal variant names and recursively equal payloads compare equal; variant identity is not used |
| `VM2A-NATIVE-SNAPSHOT-001` | `tests/golden/array_filter`, `array_count`, `array_reduce`, `array_find`; Rust callback/native tests | callback mutation is visible afterward but does not extend the current input snapshot |
| `VM2A-ERROR-LIFETIME-001` | `tests/golden/runtime_errors/array_filter_callback_failure`, `array_reduce_dynamic_callback_arity`; Rust `runtime_error_reports_inner_location_then_outer_call_site` | errors stop execution with stable locations/stack; there is no rollback or live-value result |
| `VM2A-CYCLE-DEFERRED-001` | runtime capability identified from shared vectors/cells; no accepted source contract yet | cyclic aggregate/closure graphs remain explicitly deferred and must not be silently defined by VM-2B |

## Cross-backend and verification boundary

The C++ and Rust models use different ownership primitives but must preserve the
same observable alias, equality, mutation, callback, and error behavior. The
current parity gate is:

```sh
cmake --build build
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
```

At this decision point the recorded evidence is Rust `59/59`, CTest `34/34`,
Rust VM `778/778`, canonical verification `1884/1884`, artifact `118/118`,
module cache `11/11`, malformed `108/108`, and debugger parity all passing.
The decision adds no production code and therefore has no migration or old-path
deletion in this slice.

## Explicitly deferred

- whether source programs may construct cycles, and whether the VM rejects,
  preserves, prints, or collects them;
- a stable host API for retaining runtime values or VM state after execution;
- `Heap`/handle relocation, tracing GC, weak references, cycle detection, and
  precise memory accounting;
- transactional native calls, exception recovery, or rollback after runtime
  errors; and
- concurrent VM execution, cross-thread values, and scheduler/async semantics.

## VM-2B entry gate

VM-2B may start only after an explicit architecture decision compares retaining
the current reference-counted model, wrapping it behind a `Heap`/handle facade,
or introducing another ownership strategy. The comparison must include live
and dead aggregates, closure graphs, cyclic graphs, native callback temporaries,
runtime-error exits, debug observation, peak memory, and identity stability.
Until that decision and measurements exist, no `Heap`/Handle abstraction or GC
implementation should be added under this roadmap slice.
