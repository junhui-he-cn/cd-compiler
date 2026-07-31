# VM-5B-016: inline native arguments

Status: implemented on 2026-07-31 on top of commit `6681b82b`.

## Decision

The bytecode execution loop now materializes native-call arguments as an
internal `NativeArguments` enum. Zero, one, and two arguments use inline
variants; calls with three or more arguments retain `Many(Vec<Value>)`. Native
helpers consume this container through the existing length and indexed access
shape, so arity, type validation, callback checkpoints, runtime-element
charging, and native-call diagnostics remain in their existing branches.

The test/library-facing `execute_native_call(name, Vec<Value>)` entry point
remains unchanged. It converts the owned vector at the boundary, while the
bytecode `NativeCall` path constructs the inline variants directly from
registers and does not allocate a temporary vector for the common arities.

## Compatibility and non-goals

Runtime values, native names, callback behavior, error text and locations,
profile counters, trace/debug events, resource budgets, `.cdbc 0.1`, linked and
module products, and the C++/Rust boundary are unchanged. This slice does not
change native arity contracts, introduce a native registry, borrow argument
values across dispatch, or remove the `Many(Vec<Value>)` fallback.

The fallback remains required for native calls with three or more arguments and
for the unchanged vector-based entry point. It can only be reconsidered after a
future native-arity audit demonstrates that the wider path is unreachable or
measurably hot.

## Evidence

The benchmark runner used base commit `6681b82b`, manifest revision
`bench-2026-07-30-r3`, seven repetitions, and checked stdout/stderr/exit
contracts. Toolchain: `rustc 1.94.1`, `cargo 1.94.1`, `cmake 3.28.3`.

| Workload | Before median | After median | Observation |
| --- | ---: | ---: | --- |
| `collection_helpers` | 0.001275s | 0.001347s | startup-scale noise |
| `execution_closure` | 0.248928s | 0.260526s | workload does not exercise native argument packing; host noise |
| `native_stdlib_math` | 0.001135s | 0.001142s | startup-scale noise |

Focused verification passed:

- `cargo test --manifest-path vm-rs/Cargo.toml`: `73 + 3 + 8` tests passed;
- `python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs`:
  `122/122`;
- `python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs`:
  import-order, function, and operator module sets passed;
- `python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens`:
  `786/786`;
- debugger and profile focused suites passed, including native counters,
  callbacks, source ranges, and failure reports.

