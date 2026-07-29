# M6-LANG-HASH-001: static hash capability and deterministic hash entry point

Status: implemented as the language-support slice for issue #11.

## Decision

Extend the existing compile-time capability model with `Hash`. Type parameter
bounds may contain a capability conjunction such as `T: Eq + Hash`; the
compiler canonicalizes the conjunction for function signatures, public
interfaces, and `.cdi` hashes. `Hash` does not imply `Eq`, while `Ord` retains
its existing `Eq` implication.

Expose `hash(value)` as a one-argument native operation returning a `number`.
The type checker accepts known runtime values and type parameters whose bounds
satisfy `Hash`; an unconstrained generic parameter receives a targeted
diagnostic. The operation remains a normal `native_call` in `cdbc 0.1`, so no
new opcode or runtime capability dictionary is introduced.

## Hash contract

The C++ value utilities and Rust VM use typed FNV-1a over a 32-bit result. The
algorithm uses little-endian integer fields, UTF-8 byte lengths and bytes,
canonical positive zero, and one canonical NaN bit pattern. Primitive tags are
stable; range values hash their start/stop/step, reference values hash their
runtime identity, and enum variants hash their enum/variant names and payloads.
The result is represented exactly as a non-negative `number` below `2^32`.

This slice does not make the existing map key restriction generic, does not
add `HashMap`/`HashSet`, and does not define mutable-key ownership or
user-defined capability implementations. Those decisions remain required
before hash-based library containers are added.

## Verification boundary

The focused corpus covers local, imported, namespace/re-exported, inferred,
and `Eq + Hash` generic calls; unconstrained generic rejection; public
interface text; C++ hash constants; and Rust VM execution of the emitted
native call.
