# Struct capability witness design

Issue: #17

## Question

How can a named struct's existing ordering operators satisfy generic `Eq` and
`Ord` constraints without introducing a second type representation, a
user-visible trait object, or erased generic calls that fail at runtime?

## Decision

A named struct receives a static `Ord` witness when its defining module
contains exactly one valid implementation slot for each of `<`, `<=`, `>`,
and `>=`. `Ord` continues to imply `Eq`. The existing identity equality of
struct values remains the base `Eq` behavior, so structs accepted by the
existing `T: Eq` and `T: Eq + Hash` APIs do not regress when they have no
ordering implementation. A partial operator set remains usable at direct
operator sites but does not satisfy `T: Ord`.

Witness lookup uses the existing nominal `methods_`/module-interface operator
tables. A local implementation can only belong to the module that defines the
struct; imported and re-exported interfaces forward the owner's complete
operator set and cannot create a second implementation. Existing duplicate
operator and signature diagnostics remain the conflict boundary.

## Erased generic execution

Generic function bodies are emitted once, so a generic `left < right` cannot
be lowered to one concrete method name. The compiler therefore keeps the
existing comparison instruction and emits a private deterministic binding for
each operator method of a complete witness struct:

```text
__capability_ord_<runtime-struct-name>_<less|less_equal|greater|greater_equal>
```

The Rust VM's existing comparison path uses number/string builtins as before.
For two same-named struct operands it loads that private binding from the
linked global environment and calls the ordinary operator function with the
two values. The target is an implementation detail: source code cannot name
it, no capability dictionary is exposed, and `cdbc 0.1` remains unchanged.

The target is emitted by the owning module product, so source-linked,
interface-backed, cached, and independently linked products use the same
function body. A missing target in a malformed artifact is a runtime error,
not an unchecked lookup or a host panic. Operator results are required to be
boolean at runtime, matching the static declaration contract.

## Boundaries

This slice does not add equality operator declarations, arithmetic overloads,
dynamic dispatch for arbitrary function values, trait objects, generic
monomorphization, or witness metadata to `.cdi` sidecars. Existing public
operator records already contain the complete shape needed to decide the
witness after source import, re-export, or sidecar preload.

## Verification

The proof corpus covers a complete local witness, identity equality through
`T: Eq`, rejection of a partial witness, imported and re-exported witnesses,
all four generic runtime comparisons, `.cdbc` emission and Rust execution,
independent module products, module-cache reuse/invalidation, existing
primitive ordering, and the full repository verification gates.
