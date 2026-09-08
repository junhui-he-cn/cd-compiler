# VM03-16: `cdbc 0.3` Machine Foundation ABI Freeze

Status: accepted on 2026-09-08 for the Rust VM machine-artifact
implementation line. Commit and publication are separate repository
operations and are not implied by this decision.

## Decision

Freeze the `cdbc 0.3` Machine Foundation as a VM-level and artifact-level
contract. The frozen contract is the boundary that a future compiler or LLVM
backend may target. It is not a decision to add LLVM, change the C++ compiler,
or change the default compiler artifact version.

The authoritative semantic specification is
[`docs/cdbc-0.3-machine-foundation.md`](../cdbc-0.3-machine-foundation.md).
The canonical text grammar and compatibility notes are in
[`docs/bytecode-text-format.md`](../bytecode-text-format.md).

## Version ownership

The version boundaries are deliberately split between the current compiler
and the machine-capable VM:

| Producer or consumer | Frozen behavior |
| --- | --- |
| C++ compiler | Emits linked and module `.cdbc` products as `cdbc 0.2`; no compiler-side 0.3 cutover is part of this decision. |
| C++ bytecode emitter and default compiler path | Keep the existing 0.2 wire format, opcode set, section meanings, and compatibility matrix. |
| Rust VM reader | Accepts valid `cdbc 0.2` and explicit `cdbc 0.3` artifacts; rejects unsupported versions before body interpretation. |
| Rust VM default formatter | Continues to emit the existing `cdbc 0.2` form for the default dynamic path. |
| Rust VM machine formatter | `format_program_v03` and `format_artifact_v03` explicitly emit `cdbc 0.3`. |
| Rust VM `dump` | Uses the 0.2 formatter for legacy artifacts and the machine-aware 0.3 formatter when machine fields are present. |
| Rust VM library/API version | Remains the existing 0.2 API boundary; artifact version and library API version are separate contracts. |

This means that a normal source compilation continues to use the already
verified C++/Rust `cdbc 0.2` path. A 0.3 artifact is currently produced by
the explicit Rust machine-artifact APIs and hand-built machine integration
fixtures, not by the C++ compiler.

## Frozen machine contract

The following semantics are part of the 0.3 ABI:

- `MachineInt` stores 64 raw bits. Each operation supplies an explicit width
  of 8, 16, 32, or 64 bits; signedness affects interpretation, not storage.
- `MachineFloat` supports F32 and F64 instruction formats, IEEE bit
  encodings, binary32 rounding at operation boundaries, canonical NaNs,
  signed zero, infinities, ordered and unordered comparisons, and IEEE float
  division behavior.
- `Address` is a deterministic VM virtual address. Host pointers are never
  serialized or exposed as artifact addresses.
- Linear memory is byte-addressable, little-endian, permission checked, and
  supports checked unaligned typed access. The null guard, region boundaries,
  address overflow, and range failures are VM-defined behavior.
- RODATA, DATA, and BSS have deterministic placement, alignment, payload, and
  permission rules. BSS is zero-filled and data payloads are bytes rather than
  dynamic arrays or strings.
- Machine frames are task-owned, upward-growing, 8-byte aligned, and released
  on return or trap. `FRAME_ADDR` returns an address within the live frame.
- The scalar direct-call ABI carries at most eight machine parameters and one
  optional scalar return. It has no stack arguments or indirect machine calls.
- `ABS64` data relocations resolve VM addresses. `FUNC_INDEX` relocations
  resolve VM function-table indexes and require a zero addend. Symbols and
  relocations are validated before execution.
- Typed `LOAD` and `STORE`, integer and floating-point operations, checked
  conversions, `MEMCPY`, `MEMMOVE`, and `MEMSET` use the typed trap behavior
  specified by the machine-foundation document.
- Parsing, verification, static allocation, symbol resolution, and relocation
  application all precede machine execution. Rust panics are not VM trap
  semantics.
- Machine debugger state displays domain-specific values, VM addresses, and
  frame metadata. Unsupported machine instructions remain interpreter-first;
  JIT admission falls back to the interpreter rather than emitting guessed
  host code.

The dynamic value layer, dynamic collections, module initialization, native
registry, scheduler, tracing, profiling, and existing 0.2 semantics remain
separate and are not reinterpreted as machine values.

## Compatibility and change policy

Existing `cdbc 0.2` artifacts remain unchanged. In particular, no new 0.3
field, opcode, machine section, or semantic interpretation may be encoded
under a `cdbc 0.2` header. A 0.2-only reader must reject the `cdbc 0.3`
header before interpreting its body.

Compatible 0.3 maintenance may fix implementation defects while preserving
the documented wire and execution behavior. An additive machine feature must
update the 0.3 parser, formatter, verifier, linker or VM as applicable, plus
the artifact and malformed test coverage. A breaking change to an existing
0.3 field, opcode, value representation, memory rule, relocation rule, or
call ABI requires a new version decision and migration or rejection policy.

The following remain explicitly outside the frozen 0.3 ABI:

- C++ compiler lowering and default artifact emission;
- LLVM, Clang, SelectionDAG, libc, POSIX, native object files, and dynamic
  libraries;
- heap allocation and `malloc`/`free` semantics;
- function pointers, indirect calls, varargs, and stack arguments;
- atomics, threads, TLS, SIMD, exceptions, `setjmp`/`longjmp`, and VLA;
- machine JIT lowering; and
- a binary artifact encoding or host ABI commitment.

## Acceptance evidence

The VM03-16 gate was run against the current implementation line on
2026-09-08:

| Gate | Result |
| --- | --- |
| `cargo test --manifest-path vm-rs/Cargo.toml` | 240 library tests, 4 CLI tests, 2 legacy-artifact tests, 2 workload tests, 7 lifetime tests, and 13 library API tests passed; 1 manual benchmark ignored. |
| `cmake --build build` | Passed. |
| `ctest --test-dir build --output-on-failure` | 47/47 passed. |
| `python3 tests/run_golden_tests.py ./build/compiler_design` | 875/875 passed. |
| `python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs` | 124/124 passed. |
| `python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs` | Passed. |
| `python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs` | 12/12 passed. |
| `python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens` | 776/776 passed. |
| `python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json` | 1956/1956 passed. |
| `python3 tests/run_boundary_tests.py ./build/compiler_design` | 4/4 passed. |
| `python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs --report build/malformed-report.json` | 116/116 passed. |
| `python3 tests/vm_compatibility_matrix.py` | 7/7 cells passed. |
| LSP and debugger harnesses | Passed. |

The hand-built machine integration program returns `MachineInt(12)` through
the 0.3 formatter, parser, verifier, frame-backed memory operations, and
cooperative execution path without an LLVM dependency. Separate linker tests
cover machine module expansion, symbol resolution, relocation application, and
metadata rebasing.

## Follow-up boundary

The next compiler or LLVM phase may implement lowering to this contract, but
must keep the C++ compiler's default output at `cdbc 0.2` until a separate
producer cutover decision is accepted. That cutover would require a distinct
compiler-side plan covering machine IR lowering, source-language type mapping,
artifact selection, C++/Rust parity, and migration of the existing 0.2
compatibility matrix.
