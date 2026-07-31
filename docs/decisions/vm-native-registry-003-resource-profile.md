# VM-6A-003: native resource touchpoint profile

Status: implemented on 2026-07-31 on top of commit `bbeb80af`.

## Decision

Each private `NativeSpec` now records a descriptive
`NativeResourceProfile`: `None`, `InstructionCheckpoints`, `RuntimeElements`,
or `Both`. The profile says which existing VM budget hooks a native may reach
for its supported value shapes. It is not a numeric cost and is not used to
charge the VM.

The classification is intentionally conservative. For example, `contains`
can checkpoint while scanning an array or map, but its range path does not
iterate through range values and therefore does not checkpoint. Callback
natives keep their separate `callback` marker; callback call depth is not
collapsed into this profile.

## Compatibility and non-goals

`checkpoint_native`, `charge_runtime_elements`, and
`ensure_runtime_elements` remain at their existing helper call sites. Budget
values, checkpoint timing, cancellation precedence, allocation accounting,
callback behavior, profile output, debug context, `.cdbc 0.1`, and all runtime
errors are unchanged. `NativeResourceProfile` is VM-internal and does not
become a host capability or public ABI.

This slice does not claim a uniform native cost model. It does not add runtime
cost charging, estimate input-dependent work, include host allocator bytes, or
change the instruction-step/resource-element contract.

## Evidence

The registry test classifies all 29 registered names and checks the profile
against the existing budget-touchpoint layout. Full Rust VM and cross-backend
resource tests remain the authority for actual budget behavior; no performance
threshold is introduced for metadata-only code.

## Next material boundary

Parameter and return-value constraints cannot be added safely as private Rust
metadata alone: the current `.cdbc 0.1` native call carries only a name-table
reference and runtime values, while compile-time type checking lives in the
C++ compiler. A future slice must first choose whether type descriptors are
serialized into the artifact, inferred from a versioned native ID, or kept as a
compiler-only contract. Until that shared artifact decision exists, the VM
keeps the current helper-level dynamic checks and does not invent a host ABI.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
```
