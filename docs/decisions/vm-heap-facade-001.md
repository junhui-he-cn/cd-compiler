# VM-2B-001：保留 reference counting 的 Heap facade

Status: first VM-2B facade slice implemented on 2026-07-29 against the
`a944078` VM-2A decision record. This slice does not replace runtime storage or
change the `.cdbc 0.1` contract.

## Decision

The VM keeps Rust `Rc<RefCell>` and C++ `shared_ptr`-equivalent storage as the
current ownership backend and introduces `runtime::Heap` as the single construction
boundary. `Heap` owns the VM-local identity allocators and creates functions,
arrays, maps, ranges, structs, variants, environments, and cells. `VM` owns one
Heap instance and still owns the execution roots (`globals`, active frames, and
temporary values).

This is a transitional facade, not a promise that reference counting is the
final heap strategy. `Value` and the aggregate structs retain their current
shared-storage behavior. Resource-budget charging remains in `VM` and happens
before a potentially growing Heap allocation; an identity-counter exhaustion is
reported as a stable runtime error.

All production VM creation paths now use the facade:

- main globals, frame locals, callback parameters, and captured environments use
  `Heap::new_environment`/`Heap::new_cell`;
- function, array, map, struct, range, variant, and native result construction
  uses the corresponding Heap factory; and
- map duplicate normalization remains in the Heap boundary, while the VM
  computes its normalized element charge before allocation.

The old free `new_environment`/`new_cell` functions remain compatibility helpers
for focused unit fixtures. They do not own identity or aggregate storage and
are not used by production VM execution paths.

## Candidate comparison

| Candidate | Result | Reason |
| --- | --- | --- |
| Keep direct `Rc<RefCell>` construction in `VM` | rejected as the next path | preserves behavior but leaves identity, allocation, and root entry points distributed across executor and native helpers |
| Heap facade over current reference counting | selected | centralizes construction and identity with no alias, equality, output, or error-format change; it leaves room for later accounting and handles |
| Arena/relocating handles or tracing GC now | deferred | requires cycle policy, explicit roots, host retention semantics, and live/dead/peak-memory measurements that VM-2A has intentionally left open |

The selected facade is the smallest migration step compatible with the VM-2A
contract. It does not decide cycle collection, handle stability across VM
instances, persistent host roots, or concurrent access.

## Invariants

- Identity counters start at one for each fresh VM and are not shared between
  VM instances. Allocation order and identity equality remain unchanged.
- Heap factories create fresh outer aggregate storage; cloning a returned
  `Value` still aliases the same array/map/struct storage and closure cells.
- Heap creation is not a rollback boundary. A resource error occurs before the
  corresponding VM allocation charge is committed; earlier mutations in the
  same execution remain non-transactional as specified by VM-2A.
- `Heap` is not a root registry and does not keep objects alive by itself.
  Reference counts still determine lifetime, and cycles remain deferred.
- The facade is single-threaded with the VM. `CancellationToken` signalling
  does not make Heap, `Value`, or `VM` `Send`/`Sync`.

## Verification

New Rust unit cases cover shared alias storage, distinct aggregate identities,
shared environment cells, and map duplicate ordering. Existing parity and
resource cases remain unchanged.

```sh
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
```

The first focused run after this slice passed Rust `62/62`; the existing
artifact, Rust VM, and canonical verification gates remain the compatibility
checks for the unchanged `.cdbc` and alias semantics.

## Next gate

Do not add a GC, relocating handle table, or persistent host-value API in this
slice. The next VM-2B step must add measurements for live/dead aggregates,
closure graphs, cyclic graphs, native temporary roots, runtime-error exits,
debug observations, and peak memory before deciding whether the facade remains
reference-counted or grows a different storage backend.
