# VM-6A-004: native signature shape metadata

Status: implemented on 2026-07-31 on top of commit `e2cfca8c`.

## Decision

The private native registry now records a coarse `NativeSignature` for every
registered native. Argument shapes cover the runtime checks already present in
the helpers: arrays, maps, map keys, numbers, strings, collection unions,
untyped values, and callback shapes with one or two arguments and their checked
return shape. Return shapes cover nil, primitive values, arrays, maps, ranges,
and the `any-or-nil` result of `find`.

The signature is descriptive and registry-local. `range` uses one repeated
number shape for its one-to-three numeric arguments, and `contains` uses a
conservative collection/any pair because its second operand is constrained
differently for arrays, maps, and ranges. The helper remains the executable
authority for exact value checks, callback errors, empty-container behavior,
and dynamic branches.

## Compatibility and non-goals

No native call instruction, name-table entry, artifact text, verifier rule,
runtime value representation, error text, profile output, resource policy, or
host API changes. The metadata is not serialized, does not replace dynamic
runtime checks, and does not claim to be a compiler type-system or generic
signature. In particular, it does not make the Rust VM reject an artifact based
on a type descriptor that the `.cdbc 0.1` format does not carry.

## Evidence

The registry test asserts the argument and result shape of all 29 native names,
including callback arity/return categories, `find`'s nullable result, and
`range`'s repeated numeric argument shape. Existing Rust VM, artifact, golden,
module, and malformed tests remain the behavior evidence for the executable
helper checks.

## Next boundary

Turning these shapes into centralized runtime validation or serialized artifact
constraints requires an explicit compatibility design: either add a successor
artifact field/version or define a versioned native-ID contract. Until then the
registry remains an internal audit surface and helper-level checks remain the
single source of runtime behavior.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
```
