# M6-LANG-001: static generic capability constraints for library APIs

Status: direction resolved; implementation has not started on `master`.

## Question

What language capability should be developed next so generic data-structure and
algorithm APIs can express ordering and equality without making the compiler
implement those data structures?

## Decision

Add a compile-time-only generic capability model, initially focused on
`Eq`/`Ord`-style constraints. The surface should use the existing generic-bound
position where practical, such as `T: Eq` and `T: Ord`. The implementation must
define declaration, inference, explicit arguments, nested propagation, public
interface serialization, and diagnostics as one shared semantic contract.

The first capability slice keeps explicit comparator values as a supported API
shape. A generic algorithm may receive `less: fun(T, T): bool`; the compiler must
preserve and validate that signature through local calls, imported names,
namespace-qualified calls, and re-exported functions. The capability model is a
static check and does not require a runtime trait object, implicit dictionary,
inheritance, overloading, dynamic dispatch, or a new value representation. If no
runtime semantics change, the Rust VM only needs to retain existing bytecode
execution compatibility. It also does not make generic `<` an implicit
operation; ordering laws and the
explicit comparator contract remain visible in library APIs.

## Migration and compatibility

Reuse the current generic inference, function-value checking, named-struct
methods, module graph, public interface emitter, `.cdi` validation, and module
cache. Any generic constraint present in an exported declaration must be carried
in the public interface and validated on cache hits. Direct single-file and
ordered direct-multi-file compilation retain their existing semantics.

The initial corpus is language-only. It must include positive and negative
constraint cases, inferred and explicit capability arguments, generic callback
forwarding, and cross-module/namespace/re-export comparator signatures. It must
not add `Deque`, heaps, lists, trees, graphs, sorting, sets, or other concrete
data-structure implementations as part of M6.

## Quantitative gate

Register independently named inventory cases for:

- local `Eq`/`Ord`-style generic declarations and calls;
- inferred and explicit capability arguments;
- missing-capability diagnostics and incompatible callback signatures;
- generic comparator forwarding through imports, namespace aliases, and
  re-exports; and
- public-interface, module-cache, C++ compiler, and Rust VM parity where the
  changed interface or artifact boundary is exercised.

The focused corpus must pass the relevant golden, module-interface/module-cache,
artifact, CTest, Rust VM, and Cargo checks, followed by `git diff --check`. The
gate proves language acceptance and rejection behavior; it does not require a
data-structure library implementation.

## Explicitly deferred

- a complete `Hash` protocol, generic hash containers, Bloom filters, or a
  deterministic hash/float contract;
- recursive structs, node references, ownership/alias changes, or mutable
  recursive trees and linked lists;
- a separate integer type, arbitrary-precision integers, or new overflow
  exceptions;
- dynamic dispatch, inheritance, overloading, runtime trait objects, and
  static methods; and
- a library-specific test runner, which is independent test infrastructure.

The corresponding library decisions, including nullable versus `Option` versus
`Result`, ring-buffer full behavior, `number` boundaries, and integer graph IDs,
are recorded in [`library/DATA_STRUCTURES_ROADMAP.md`](../../library/DATA_STRUCTURES_ROADMAP.md).
