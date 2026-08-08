# V6C: VM helper ABI and safepoint materialization bridge

Status: implemented on 2026-08-07 as the second V6C preparation slice. This
slice defines the internal boundary required by a future Cranelift execution
tier, but does not execute generated code or change the public VM contract.

## Scope

`vm-rs/src/jit.rs` now owns the first stable internal helper descriptors:

- helper IDs are explicit and unique for the admitted scalar subset;
- every helper receives one VM context handle, zero or more opaque 64-bit
  operands, and one opaque 64-bit result handle; and
- Cranelift import signatures are built from the same descriptor used by the
  ABI tests, so helper arity cannot silently drift from IR lowering.

The frame boundary is represented by `JitFrameMaterialization`. It owns cloned
register values, bytecode body identity, instruction position, locals, closure,
function identity, return target, task identity, and safepoint kind. It retains
no raw pointers or active `RefCell` borrows and can restore the captured state
into the existing `ResumableFrame` contract.

`VM::jit_safepoint` is the VM-owned bridge. It materializes the frame before
running the shared instruction/native checkpoint. A limit or cancellation
failure therefore still has a borrow-free fallback frame. Scheduler, garbage
collection, error, and return categories only capture state in this slice;
their owning VM/scheduler paths remain responsible for the corresponding
transition. Cooperative and observable sessions remain interpreter-only.

`VM::jit_helper_bridge` now provides the matching typed dispatcher for the
initial helper set. It validates descriptor-derived arity and 64-bit opaque
handles, keeps temporary `Value` results in a bridge-local root table, and
reuses the VM's constant/load, arithmetic, equality, and ordered-comparison
semantics. The table is released with the bridge; it is not a persistent cache
or an artifact-visible handle space.

## Compatibility boundary

There is still no JIT execution switch, public Rust type, CLI option, artifact
field, executable memory, machine-code cache entry, or `.cdbc 0.1` change.
The interpreter remains the only production execution path. The helper ABI is
an internal Cranelift preparation boundary, not an FFI or plugin ABI.

## Verification

Focused Rust tests cover helper ID/arity stability, frame capture and restore,
materialization before resource-limit and cancellation failures, and the fact
that non-budget safepoints do not consume instruction steps. The existing VM,
artifact, cooperative, lifecycle, and library tests remain unchanged and
continue to exercise interpreter behavior.

The pre-executable entry lifetime and rollback guard is recorded in
[`v6c-jit-code-lifetime-rollback-001.md`](v6c-jit-code-lifetime-rollback-001.md).
The next decision is whether the measured `execution_closure` subset justifies
an optional Cranelift execution path. Any such slice must connect only through
the materialized-frame and typed-helper boundaries above, and prove that a
failed or expired compiled entry returns to the interpreter without losing
roots, diagnostics, resource checks, or observability. No interpreter call is
replaced by either preparation slice.
