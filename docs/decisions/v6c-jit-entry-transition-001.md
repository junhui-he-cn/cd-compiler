# V6C: VM-owned x86-64 entry transition

Status: implemented on 2026-08-08 as a private execution slice. This slice
connects the existing VM-local Cranelift backend to ordinary function calls
only when an internal test state explicitly enables the JIT. `VM::new` and all
public observable/cooperative paths remain interpreter-controlled.

## Scope

The generated scalar subset now crosses a VM-owned entry transition:

- `VM::call_function` constructs the existing callee `Frame`, binds parameters,
  and admits only the existing verified whitelist;
- Cranelift emits a `Checkpoint` helper before every admitted bytecode
  instruction and a `StoreRegister` helper after every register-producing
  instruction;
- the helper bridge keeps opaque `Value` handles rooted, materializes the
  current `ResumableFrame` before checkpoint charging, and records a return
  safepoint after successful generated execution;
- runtime and resource failures are transported through the C ABI error slot,
  then receive the same bytecode location and call-stack decoration as the
  interpreter; and
- an entry snapshot restores frame state and the instruction counter when an
  internal protocol failure, stale invocation, or invalid result requires
  interpreter fallback.

The generated entry receives argument handles through the x86-64 System V ABI,
while function parameters remain authoritative in the VM-owned local cells.
Runtime values are not specialized to native numbers or serialized into
`.cdbc` artifacts.

## Compatibility boundary

The transition is not a public JIT switch. `JitState::enabled_for_tests` is the
only activation point, and it is compiled only for focused Rust tests. Trace,
profile, debugger, cooperative, native, callback, and unsupported bytecode
paths use the interpreter. The initial machine-code subset remains the
non-speculative scalar operations already admitted by `jit.rs`.

Checkpoint charging preserves cancellation-before-limit ordering because the
bridge calls the existing `VM::checkpoint_instruction`. Protocol fallback
rewinds only the frame and VM instruction counter; the admitted subset has no
native, output, aggregate mutation, or callback operation before that boundary.

## Verification

Focused VM tests cover:

- repeated execution of a whitelisted two-argument function through finalized
  x86-64 code and frame register stores;
- identical instruction-limit kind, limit, message, and step count between
  JIT and interpreter execution;
- restoration of the entry `ip`, register vector, and checkpoint counter after
  a synthetic register-protocol failure; and
- the existing helper, safepoint, cache-owner, executable-lifetime, and
  interpreter fallback tests.

The next gate is not an optimizing tier. It is an explicit parity decision for
native/GC/debug/profile/cooperative boundaries and a measured choice about
whether this private path should become a user-visible opt-in.
