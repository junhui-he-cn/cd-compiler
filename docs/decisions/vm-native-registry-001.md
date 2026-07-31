# VM-6A-001: native registry dispatch boundary

Status: implemented on 2026-07-31 on top of commit `cacf3803`.

## Decision

The Rust VM now keeps one private `NativeSpec` table for the 29 native names
currently emitted by the compiler. Each record has an internal `NativeId`, the
artifact-facing name, the current minimum and maximum arity, and whether the
native invokes a callback. `execute_native_call_at` resolves the name through
this table and dispatches on the internal ID instead of matching the artifact
name in the execution branch itself.

The registry is descriptive at this stage. Existing native helpers remain the
authority for argument validation, callback validation, resource checkpoints,
and their human-readable errors. This avoids validating the same call twice or
changing the established error text while the registry contract is introduced.

## Compatibility and non-goals

Unknown names still fail before execution with
`unknown native stdlib function \`<name>\``. Registered native behavior,
callback call sites, profile names and counts, resource limits, debug context,
`.cdbc 0.1`, and the artifact allowlist remain unchanged. The registry does not
expose Rust function pointers, accept dynamic plugins, or create a host ABI.
Parameter/return type descriptors, centralized arity enforcement, resource
cost policy, and host capability injection remain later VM-6A/VM-6B slices.

## Evidence

The Rust unit tests assert that all 29 entries have unique names, resolve back
to their records, preserve the range arity and callback markers, and reject an
unregistered name with the legacy error text. Existing artifact verification
continues to reject unsupported native names before execution.

## Next gate

The next narrow slice may consume the registry's arity metadata at the common
dispatch boundary, but only after preserving each helper's current diagnostic
wording and callback-specific validation. Resource policy remains descriptive
until a separate measured contract identifies native operations whose budget
cost can be centralized without changing checkpoint timing.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
```
