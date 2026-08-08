# V6C: x86-64 baseline JIT backend

Status: implemented on 2026-08-08 as the executable-code generation slice of
V6C. This slice adds a VM-local Cranelift host backend and keeps production VM
execution interpreter-controlled.

## Scope

`vm-rs/src/jit.rs` now owns a private `JITModule` backend using Cranelift
`0.134.3`:

- the host target is x86-64 with the existing System V calling convention;
- the admitted scalar bytecode subset is lowered into IR with module-declared
  helper imports rather than unresolved synthetic external names;
- helper symbols resolve to typed C ABI stubs, which forward an opaque 64-bit
  context and opaque value handles to a caller-owned dispatch table;
- finalized function addresses and the generated machine-code byte count are
  retained beside the verified IR in the VM-local bounded cache; and
- a cache clear invalidates all old handles, releases the old executable
  mapping, and creates a fresh helper-import module.

The initial entry ABI supports a context plus zero through eight 64-bit
arguments. Functions with larger arity return a structured interpreter
fallback reason until a packed-argument ABI is specified. The backend does
not specialize CD values to native numbers; runtime semantics remain in the
helper dispatch boundary.

## Lifetime and compatibility

The backend is created only for an explicitly enabled internal test state.
`JitState::disabled()` remains the production default, and no CLI flag,
public Rust configuration, `.cdbc 0.1` field, or machine-code serialization is
added. Trace, profile, debugger, cooperative, resource, cancellation, and
ordinary `VM::run` paths remain interpreter-controlled.

A code pointer is returned only after the cache verifies the VM owner and
generation. The pointer is never stored in an artifact or shared between VM
instances. Clearing the cache is only valid when no generated entry is
executing; the old handles are invalidated before the module memory is freed.

## Verification

The focused x86-64 test finalizes a function containing `LoadVar` and `Add`,
invokes the generated machine code through the opaque helper context, and
checks both the returned handle value and helper call count. Existing cache,
rollback, helper ABI, safepoint, interpreter, scheduler, artifact, and library
tests remain part of the VM verification contract.

## Next boundary

Connecting this backend to ordinary function-call execution still requires a
VM-owned entry transition that constructs the callee frame, materializes roots
and debug state, charges shared checkpoints, transports helper errors, and
returns to the interpreter on failure. That integration remains opt-in and
must be proven before the backend replaces any production interpreter call.
