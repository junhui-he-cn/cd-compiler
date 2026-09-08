# Compiler Design VM Roadmap

Current-state record for the Rust VM in `vm-rs/`. All previously planned VM
slices are void as of 2026-08-15: completed work lives in decision records and
Git history. The active cdbc 0.3 Machine Foundation iteration is governed by
[`plan-0.3.md`](plan-0.3.md); VM03-00 is specified in
[`cdbc-0.3-machine-foundation.md`](cdbc-0.3-machine-foundation.md), and
VM03-01/VM03-02/VM03-03 now provide the in-memory machine value and integer,
floating-point, and mixed-conversion execution foundation. VM03-03 adds F32/F64
arithmetic, ordered/unordered comparisons, IEEE division behavior, canonical
NaNs, and checked integer/float conversion traps. VM03-04 now adds the
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
atomic range/permission validation. VM03-09 now adds a checked register-facing
machine ABI with
`MachineInt`, `MachineFloat`, and `Address` scalar domains, up to eight
direct-call parameters, one optional scalar return, explicit indirect-call
rejection, linker metadata preservation, and interpreter fallback when JIT is
enabled. VM03-10 now adds VM-level function and data symbols, `ABS64` data
patches, `FUNC_INDEX` direct-call relocations, module-linker index rebasing,
duplicate/undefined-symbol rejection, and loader ordering that allocates
segments and resolves all symbols before applying patches. VM03-11 now adds a
strict Rust-side `cdbc 0.3` reader and explicit machine-artifact writer, with
round-trip support for machine opcodes, segment payloads, ABI metadata,
symbols, and relocations; the existing C++ emitter and default formatter stay
on `cdbc 0.2` until the compiler-side artifact cutover is separately chosen.
VM03-12 now adds typed machine traps for arithmetic, shifts, conversions,
instruction and operand failures, plus load-time verifier coverage for machine
types, widths, registers, memory and frame metadata, global slots, symbols,
relocations, and call ABI boundaries. The loader keeps the same invalid-product
classification for unverified in-memory programs and avoids host panics while
handling malformed entry, branch, frame, and machine-linkage state.
VM03-13 now exposes machine register values and active frame base/size through
debugger pauses and makes the VM `dump` command render verified machine
artifacts with the canonical `cdbc 0.3` disassembly while preserving `cdbc 0.2`
output for legacy artifacts.
VM03-14 now keeps cdbc 0.3 machine instructions and machine-ABI functions out
of the current JIT admission set, returning them to the existing interpreter;
an end-to-end parity test covers the fallback result, instruction accounting,
and empty JIT cache. No machine JIT lowering is implied.
VM03-15 now exercises a hand-built machine program through cdbc 0.3 formatting,
parsing, verification, and cooperative execution; its frame-backed four-value
array/Point equivalent returns `MachineInt(12)` without an LLVM dependency.
VM03-16 now freezes the VM-level cdbc 0.3 Machine Foundation ABI for this
implementation line. The freeze covers machine values, memory, frames, data
segments, scalar calls, symbols/relocations, traps, debug display, and JIT
fallback behavior. It does not change the C++ compiler or default artifact
emission, which remain on cdbc 0.2. Machine execution remains
interpreter-first.
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

The C++ compiler and default artifact path remain `cdbc 0.2`, with unchanged
CLI text/exit behavior, deterministic execution, current resource accounting,
and C++/Rust parity. The Rust VM additionally accepts explicit, validated
`cdbc 0.3` machine artifacts under the frozen contract recorded in
[`cdbc-0.3-machine-abi-001`](decisions/cdbc-0.3-machine-abi-001.md).

## Current shipped baseline

| Area | Shipped baseline |
| --- | --- |
| Artifact safety | Shared `cdbc 0.2` parser/formatter/verifier, explicit `cdbc 0.3` machine parser/formatter/verifier, malformed corpus, resource limits, cancellation |
| Execution | Register VM for the complete emitted instruction set and native surface |
| Runtime values | Stable identity-bearing storage, non-moving tracing collection at VM safepoints, recursive values, cycle-safe formatting, in-memory cdbc 0.3 machine scalar values, typed/bulk machine memory, and task-owned machine frames |
| Modules | Deterministic module validation/linking, debug rebasing, typed errors, optional link report |
| Embedding | Rust library parse/verify/link/run/trace/debug/profile API plus CLI adapters |
| Observability | Interactive debugger, deterministic counters, tracked heap counts, estimated retained bytes, structured error kinds |
| Performance/capacity | Reproducible phase benchmark, scaled workloads, capacity and budget corpus, artifact-load and format-capacity evidence |
| Native boundary | Private registry with arity, callback, resource-touchpoint, and signature-shape metadata |

The authoritative behavior contracts live in the tests, decision records,
`docs/bytecode-text-format.md`, `docs/cdbc-0.3-machine-foundation.md`, and
`vm-rs/README.md`; the verification command set lives in `AGENTS.md`.
