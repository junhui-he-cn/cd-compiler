# VM-6A-002: centralized native arity validation

Status: implemented on 2026-07-31 on top of commit `906acffe`.

## Decision

Each private `NativeSpec` now carries the exact legacy arity diagnostic in
addition to its minimum and maximum argument counts. The common native dispatch
boundary performs the count check after the existing profile counter hook and
before dispatching on `NativeId`. Native helpers no longer repeat their
top-level argument-count checks.

The registry validates only the number of arguments. Helpers continue to own
value-shape checks, callback function checks and callback arity, range-specific
semantics, resource checkpoints, mutation, and return values. This preserves
the distinction between native call arity and callback arity.

## Compatibility and non-goals

All existing arity messages, including their historical singular/plural forms,
remain unchanged. Invalid calls still count in profile mode because validation
runs after `profile_native_call`. Unknown names are still rejected before the
profile hook and before arity validation. The artifact format, verifier
allowlist, callback behavior, resource timing, debug context, `.cdbc 0.1`, and
host ABI are unchanged.

This slice does not add parameter or return type descriptors, change native
resource charging, or expose the registry to embedders. Resource-cost metadata
requires a separate contract because native operations have different loop,
allocation, and callback checkpoint behavior.

## Evidence

The registry tests exercise every registered native with zero arguments and
assert the exact `NativeSpec` diagnostic. The existing native arity/type,
callback, range, profile, resource, artifact, and Rust VM tests remain the
behavioral coverage for valid calls and non-arity failures.

## Next gate

The next VM-6A slice should decide whether any resource-cost metadata can be
descriptive without pretending to replace the existing checkpoint policy. It
must be backed by a measured workload and keep callback iteration and allocation
limits deterministic; otherwise resource policy remains in the helpers.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
```
