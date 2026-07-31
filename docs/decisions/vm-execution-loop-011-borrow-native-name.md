# VM-5B-011: borrowed native-call name

Status: implemented on 2026-07-31 on top of the borrowed read-only register
operands slice.

## Decision

The bytecode `NativeCall` instruction now reads its native name through the
immutable `Program` name table and passes it as `&str` for the duration of
dispatch. The helper's `VM<'a>` name lookup explicitly returns the program
owned lifetime, so the name borrow does not remain tied to a short-lived
`&self` borrow while native dispatch mutates the VM.

Native profiling still copies the name only when recording a `BTreeMap` key;
native errors and all existing native helper contracts remain unchanged.

## Compatibility and non-goals

Native dispatch names, shadowing behavior, callback/resource checkpoints,
runtime diagnostics, trace/debug/profile output, `.cdbc 0.1`, and public
VM/library APIs remain unchanged. This slice does not redesign the native ABI,
registry, or name table, and other ownership-requiring name paths continue to
copy.

## Evidence

The benchmark runner used base commit `8bfc5bbb` with the 009/010 changes held
constant in the working tree, manifest revision `bench-2026-07-30-r3`, three
repetitions, and checked stdout/exit contracts:

| Workload | Before median | After median | Observation |
| --- | ---: | ---: | --- |
| `collection_helpers` | 0.001312s | 0.001244s | startup-scale improvement |
| `native_stdlib_math` | 0.001121s | 0.001125s | startup noise |

Cargo tests and both focused benchmark workloads passed after the change.

