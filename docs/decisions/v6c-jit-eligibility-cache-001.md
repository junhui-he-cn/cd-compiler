# V6C: Cranelift IR admission and bounded cache

Status: implemented on 2026-08-07 as the first V6C preflight slice. This
slice selects Cranelift as the JIT IR framework, but does not generate machine
code, add executable memory, or change the public VM API.

## Scope

`vm-rs/src/jit.rs` adds a private admission boundary owned by each `VM`:

- JIT is disabled by default, so ordinary execution remains interpreter-only.
- An enabled state requires an explicit `Program.functions` whitelist and the
  ordinary, non-observable execution mode.
- The initial eligibility subset admits constants, moves, variable loads,
  scalar unary/binary operations, comparisons, and returns.
- The main body, non-whitelisted functions, dynamic calls, native calls,
  callback-capable native calls, unsupported instructions, trace/profile/debug
  modes, and cooperative sessions return structured interpreter fallback
  reasons.
- Eligible functions lower through `cranelift-frontend`'s
  `FunctionBuilder` into verified `cranelift-codegen` IR. Dynamic CD values
  remain opaque `i64` handles; constants, variable loads, unary operations,
  and binary operations are represented as calls to a future VM-owned runtime
  helper bridge rather than being incorrectly specialized to native numbers.
- A finite per-VM cache accounts for admitted code reservations, reuses an
  existing Cranelift IR entry, refuses over-budget reservations, and can be
  explicitly cleared. It is separate from artifact and runtime-element
  budgets and has no serialization path.

The current state is a policy/cache and IR owner only. It is intentionally not
wired to replace `execute_body`, `execute_instruction`, or the cooperative
frame loop, and it contains no executable memory or machine-code entry point.

## Compatibility boundary

There is no CLI flag, `RunConfig` field, exported Rust type, `.cdbc 0.1`
field, native name, or artifact change. Existing `run`, trace, profile,
debugger, cooperative, error, resource, and C++/Rust parity behavior remains
interpreter-controlled.

The cache is a field of `VM`, so it cannot be shared between VM instances and
is released with the VM. Its finite reservation remains an explicit estimate
for the future Cranelift machine-code unit; the current cached object is the
verified IR and makes no claim about machine-code size or runtime speed.

## Verification

Unit tests cover disabled behavior, whitelist and mode gates, main-body and
unsupported-unit fallback, dynamic/native/callback boundaries, Cranelift IR
construction and verification, bounded cache admission, reuse, eviction, and
VM-local ownership. The next V6C slice must define the VM helper ABI and frame
materialization/safepoint bridge before adding `cranelift-jit` execution or
allowing any compiled path to replace the interpreter.
