# Compiler Design VM Roadmap

Current-state record for the Rust VM in `vm-rs/`. All previously planned VM
slices are void as of 2026-08-15: completed work lives in decision records and
Git history. The active cdbc 0.3 Machine Foundation iteration is governed by
[`plan-0.3.md`](plan-0.3.md); VM03-00 is specified in
[`cdbc-0.3-machine-foundation.md`](cdbc-0.3-machine-foundation.md), and
VM03-01/VM03-02 plus the integer-conversion portion of VM03-03 now provide the
in-memory machine value and integer execution foundation. VM03-04 now adds the
VM-owned deterministic `LinearMemory` region allocator, VM address/range
checking, region permissions, and the memory error categories. VM03-05 now adds
interpreter execution for typed `LOAD`/`STORE` across integer, floating-point,
and address domains, including little-endian unaligned access and the existing
memory trap mappings. VM03-06 now attaches checked machine frame metadata to
the existing call stack, allocates upward-growing 8-byte-aligned frames, adds
`FRAME_ADDR`, unwinds frames on return and traps, and gives each cooperative
task an independent machine stack. VM03-07 now adds in-memory RODATA, DATA,
and BSS descriptors, deterministic static placement, initialization/zero-fill,
and segment permission validation before execution; linked module products
carry their segment descriptors in expansion order. VM03-08 now adds checked
interpreter execution for `MEMCPY`, `MEMMOVE`, and `MEMSET`, including
zero-length operations, overlap-safe moves, deterministic overlap traps, and
atomic range/permission validation. Floating-point and mixed integer/float
conversions from VM03-03 remain pending. Machine execution remains
interpreter-first; neither machine instructions nor segment descriptors are
part of the cdbc 0.2 text artifact writer.
The compiler, language, and compiler tools have a separate current-state record in
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
| Runtime values | Stable identity-bearing storage, non-moving tracing collection at VM safepoints, recursive values, cycle-safe formatting, in-memory cdbc 0.3 machine scalar values, typed/bulk machine memory, and task-owned machine frames |
| Modules | Deterministic module validation/linking, debug rebasing, typed errors, optional link report |
| Embedding | Rust library parse/verify/link/run/trace/debug/profile API plus CLI adapters |
| Observability | Interactive debugger, deterministic counters, tracked heap counts, estimated retained bytes, structured error kinds |
| Performance/capacity | Reproducible phase benchmark, scaled workloads, capacity and budget corpus, artifact-load and format-capacity evidence |
| Native boundary | Private registry with arity, callback, resource-touchpoint, and signature-shape metadata |

The authoritative behavior contracts live in the tests, decision records,
`docs/bytecode-text-format.md`, and `vm-rs/README.md`; the verification
command set lives in `AGENTS.md`.
