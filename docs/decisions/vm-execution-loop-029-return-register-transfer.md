# VM-5B-029: transfer the return value out of a dead frame

Status: implemented on 2026-08-01 on top of commit `227c905c`.

## Decision

The bytecode `Return` instruction now moves its value out of the current frame
register with `std::mem::replace`, leaving that dead register as `nil`. The
returned `Value` becomes the owned result of `execute_body`; no clone is needed
because the frame is not resumed after a return.

The transfer preserves shallow aggregate identity: moving an array, map,
struct, function, or closure value transfers the same runtime handle rather
than copying its storage. Primitive values move directly. Invalid register
indices still produce the existing runtime error.

## Compatibility and non-goals

Function and entry return values, aliasing, closure capture, call stacks,
trace/debug return events, resource checkpoints, profile counters, `.cdbc 0.1`,
and CLI output remain unchanged. This slice only changes the ownership of a
register in a frame that has already terminated. It does not move values from
live registers, alter `Move`, or change Call/native argument transfer.

## Evidence

The focused transfer unit test, Cargo unit/CLI/library suites, bytecode artifact
tests, Rust VM goldens, debugger/profile checks, and seven-sample closure/loop/
index/native/error benchmark matrix pass. Existing function and aggregate
return fixtures remain output- and alias-compatible.

## Deletion condition

Keep the transfer while `execute_body` never resumes a frame after returning. If
future stack inspection, resumable execution, or post-return frame observers
need the register value, restore an explicit ownership boundary and define that
observer contract before reusing this optimization.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_rust_vm_tests.py build/compiler_design vm-rs --goldens
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py build/compiler_design vm-rs --repeat 7 --workload execution_closure --workload execution_loop --workload execution_index --workload collection_helpers --workload runtime_error
```
