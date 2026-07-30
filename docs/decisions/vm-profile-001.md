# VM-4B-001：deterministic execution profile boundary

Status: first deterministic counter slice implemented on 2026-07-30 on top of
the library and debugger boundaries. The profile is opt-in and observes one
execution without changing the existing `.cdbc 0.1` artifact or VM execution
contract.

## Decision

The library exposes:

```rust
VM::profile() -> ProfileRun
```

`ProfileRun` contains a typed `result` and a `ProfileReport`. The report is
returned for both successful and failed executions and contains:

- `instruction_count`: bytecode instructions whose instruction checkpoint
  succeeded;
- `output_bytes`: UTF-8 bytes successfully appended to the VM output buffer;
- `functions`: `main` followed by artifact functions in definition order, with
  calls and executed instruction counts;
- `natives`: invoked native names in lexicographic order and their call counts;
- `source_ranges`: hit `DebugRange` values sorted by `(source, start, end)`.

The `index` of the entry body is `None`; artifact functions retain their
bytecode function index. Function calls are counted when a body is entered,
including the entry body. Native calls are counted at native dispatch,
including a dispatch that later rejects its arguments. Native callback
iteration checkpoints are resource-budget charges, not bytecode instruction
counts. A bytecode instruction that passes its checkpoint is counted even if
its operation subsequently raises a runtime error; an instruction rejected by
the checkpoint is not counted. A source range is hit once for each counted
instruction carrying that range. Ranges without hits are omitted.

The CLI command is:

```text
compiler-design-vm profile <program.cdbc>
```

It prints stable line-oriented records:

```text
profile status=ok
profile instruction_count=... output_bytes=...
profile function index=main name="main" calls=... instructions=...
profile native name="..." calls=...
profile source_range source=s0 path="..." start=... end=... hits=...
```

Failed execution still prints the status and collected report to stdout, then
uses the existing runtime/resource diagnostic on stderr and exits non-zero.
The executed program's output is never printed by `profile`; only its byte
count is reported. Names and paths use the existing trace escaping helper.

## Compatibility and non-goals

Profiling is disabled for `run`, `trace`, and `debug`, so their output, trace
sequence, debugger pauses, resource counters, and metadata-free behavior are
unchanged. No artifact field or native name is added. The report is intended
for deterministic correctness and coverage evidence, not as a JIT input.

This slice deliberately does not report wall-clock execution time: host clock
resolution and scheduling would make the primary report nondeterministic. It
also does not report allocation or peak bytes; the VM-2 heap measurement
contract still needs workload-level decisions. Line coverage without an
existing `DebugRange`, persistent sessions, snapshots, and profile file
schemas remain deferred.

## Migration and deletion condition

The profile API is additive and has no compatibility path to delete. The
existing output buffer, trace collector, and resource ledger remain the
owners of their respective semantics. A future timing or allocation report
must use a separately versioned field/schema decision rather than silently
changing this deterministic record set.

## Evidence

The library integration tests cover successful function/native/range/output
profiles and a runtime failure with partial output accounting. The CLI test
covers repeatability, native counters, source paths, output separation,
runtime failures, and instruction-budget failures:

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/profile_tests.py ./build/compiler_design vm-rs
```
