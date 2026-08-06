# V5B: repeatable cooperative workload evidence

Status: implemented on 2026-08-06. This slice establishes deterministic
multi-task workload evidence for the completed V5B host observability surface;
it does not measure wall-clock performance or begin V5C/V6 implementation.

## Scope

`vm-rs/tests/cooperative_workloads.rs` is an integration-level host harness
that constructs verified in-memory `Program` values and drives the public
cooperative API. It covers two representative workloads:

- `arithmetic_worker`: four tasks with different loop bounds, output, and
  repeated FIFO quantum dispatch;
- `callback_worker`: two tasks using `map` and a nested identity callback,
  exercising native transitions and callback frames.

Each workload is run through the opt-in profile, trace, and debugger surfaces
where applicable. The harness records only deterministic evidence:
dispatch count, output and task-output events, profile counters, trace events,
debug pauses, and typed task-outcome renderings.

## Evidence contract

- Repeating the same workload with the same quantum produces byte-for-byte and
  record-for-record equal observations.
- Arithmetic profile/output/outcome observations remain equal for quantum 1,
  3, and 32, showing that cooperative quantum changes scheduling granularity
  without changing the execution contract.
- Callback observations retain task identity across native `map` invocation,
  nested callback trace frames, and debugger pauses.
- The harness does not record wall-clock time, infer throughput, or claim a
  JIT speedup. Existing CLI benchmark timing remains a separate informational
  tool and is not mixed with host scheduler evidence.
- No language task syntax, artifact field, OS thread, `Send`/`Sync` promise, or
  `.cdbc 0.1` change is introduced.

## Verification

```sh
cargo test --manifest-path vm-rs/Cargo.toml --test cooperative_workloads
cargo test --manifest-path vm-rs/Cargo.toml cooperative_
git diff --check
```

## Next boundary

V5B now has deterministic task lifecycle, output, trace, profile, debugger, and
workload evidence. The next decision is whether a concrete consumer justifies
optional V5C concurrency expansion; independently, V6A may characterize a
measured hot bytecode workload only after stable profiles and reproducible
timing evidence are collected. No JIT code belongs in this slice.
