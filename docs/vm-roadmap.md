# Compiler Design VM Roadmap

Current-state record for the Rust VM in `vm-rs/`. All previously planned VM
slices are void as of 2026-08-15: completed work lives in decision records and
Git history, and future work starts from a fresh explicit decision. The
compiler, language, and compiler tools have a separate current-state record in
[`docs/roadmap.md`](roadmap.md).

## VM product boundary

The VM is a deterministic, validated, observable `.cdbc` execution engine. It
must:

- reject malformed, unsupported, over-budget, or unlinkable products before
  unsafe execution state is observed;
- preserve compiler-defined value, alias, call, native, output, and error
  semantics;
- expose equivalent CLI and Rust library behavior;
- keep ordinary execution independent from opt-in trace, debug, profile, and
  capacity instrumentation; and
- provide evidence before changing storage, performance, or artifact policy.

The VM does not independently invent language syntax, type semantics, module
cache invalidation, async semantics, or compiler optimization policy.

## Compatibility constraints

`cdbc 0.2` emission and execution only, CLI text/exit behavior, deterministic
execution, current resource accounting, and C++/Rust parity remain
compatibility constraints.

## Current shipped baseline

| Area | Shipped baseline |
| --- | --- |
| Artifact safety | Shared `cdbc 0.2` parser/formatter/verifier, malformed corpus, resource limits, cancellation |
| Execution | Register VM for the complete emitted instruction set and native surface |
| Runtime values | Stable identity-bearing storage, non-moving tracing collection at VM safepoints, recursive values, cycle-safe formatting |
| Modules | Deterministic module validation/linking, debug rebasing, typed errors, optional link report |
| Embedding | Rust library parse/verify/link/run/trace/debug/profile API plus CLI adapters |
| Observability | Interactive debugger, deterministic counters, tracked heap counts, estimated retained bytes, structured error kinds |
| Performance/capacity | Reproducible phase benchmark, scaled workloads, capacity and budget corpus, artifact-load and format-capacity evidence |
| Native boundary | Private registry with arity, callback, resource-touchpoint, and signature-shape metadata |

The authoritative behavior contracts live in the tests, decision records,
`docs/bytecode-text-format.md`, and `vm-rs/README.md`; the verification
command set lives in `AGENTS.md`.
