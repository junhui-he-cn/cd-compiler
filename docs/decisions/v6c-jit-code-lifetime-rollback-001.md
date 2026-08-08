# V6C: Compiled-entry lifetime and rollback guard

Status: implemented on 2026-08-08 as the third V6C preparation slice. This
slice makes cache entry ownership and invalidation explicit without adding
executable memory or replacing an interpreter call.

## Scope

`vm-rs/src/jit.rs` now gives every admitted IR entry a VM-local
`JitCodeEntryHandle` containing:

- the function index;
- a monotonic cache generation; and
- an owner token unique to the VM-local cache.

`JitAdmission::Reserved` and `JitAdmission::Cached` return the handle that a
future execution tier must resolve immediately before use.
`JitState::resolve_code` rejects handles from another VM and handles whose
entry was cleared or replaced, returning structured interpreter fallback
reasons. The handle has no machine-code address and does not keep a VM or
program alive.

Admission is publish-after-verify: eligibility and Cranelift IR lowering finish
before the cache reserves and publishes an entry. A lowering failure therefore
does not consume cache bytes or leave a partially published entry. Explicit
cache clearing invalidates old handles; a subsequent admission receives a new
generation.

## Compatibility boundary

The interpreter remains the only production execution path. There is no
`cranelift-jit` dependency, executable-memory allocator, machine-code pointer,
serialized entry, public API, CLI option, or `.cdbc 0.1` change. JIT remains
disabled by default and observable/cooperative execution remains on the
interpreter path.

## Verification

Focused Rust tests cover successful handle resolution, cross-VM rejection,
stale-handle fallback after eviction, replacement generation changes, and
rollback of cache accounting after failed IR publication. Existing helper,
safepoint, cache, VM-local ownership, artifact, and Rust VM parity gates remain
the compatibility checks for this preparation boundary.

The next boundary is an explicit decision about a real Cranelift execution
backend, calling convention, executable-memory lifetime, and parity proof. No
compiled entry is executable until that decision and its backend-specific
tests exist.
