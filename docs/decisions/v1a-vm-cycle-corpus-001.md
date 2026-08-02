# V1A: VM cycle corpus and measurement

Status: resolved on 2026-08-02 at the source/compiler baseline committed as
`a78cce37`. This is a measurement and evidence slice; it does not select or
implement a storage policy.

## Decision

Source programs may construct self and mutual cycles through the existing
array, map, and recursive-struct operations. Recursive closures also create a
supported environment cycle. The current `Rc<RefCell<...>>` ownership remains
the observed behavior: a strong cycle stays live after its external roots are
dropped, while replacing the back-edge with `nil` releases the storage.

`HeapStats` remains an observational, weak-ledger API. The corpus records
tracked allocation totals, live/dead counts, peak live counts, and estimated
retained bytes. The estimate is VM-owned representation pressure, not an
allocator or host-RSS contract. RSS and elapsed time may be printed as
observations, but are not pass/fail thresholds.

## Corpus

| Workload | Source evidence | Expected observation |
| --- | --- | --- |
| self array | `let xs = []; push(xs, xs)` | cycle-safe `[<cycle>]` output |
| self map | map self-index assignment | cycle-safe `map{self: <cycle>}` output |
| self struct | recursive `Node.next = node` | cycle-safe struct output |
| mutual struct | two recursive `Node` values linked both ways | cycle marker on the repeated node |
| closure/environment | recursive nested `count` closure | deterministic result with captured environment pressure |
| replacement | array, map, and struct back-edges replaced with `nil` | normal acyclic output and released storage in the direct heap test |
| runtime failure | retained cycle followed by division by zero | typed runtime failure and partial profile report |

The Rust integration test also drops all external roots from one array cycle,
one map cycle, one self-struct cycle, one mutual-struct cycle, and one
closure/environment cycle. The weak ledger observes seven live tracked
storages, then observes zero live and seven dead after each back-edge is
replaced. Sixteen in-process VM instances execute the same cyclic program and
produce byte-for-byte equal profile reports.

## Compatibility and non-goals

The C++ compiler still emits `cdbc 0.1`; the Rust VM still owns execution,
cycle-safe formatting, diagnostics, and profile output. No artifact section,
native name, CLI command, library version, or runtime value identity rule
changed in V1A.

This slice does not add GC, weak-reference syntax, relocating handles, cycle
detection/rejection, finalizers, a persistent session, or a host memory
schema. Those choices belong to V1B and must be made from this evidence.

## Evidence

The source-backed matrix is implemented by
`tests/vm_cycle_tests.py`; direct ownership and in-process observations are in
`vm-rs/tests/cycle_lifetime.rs`.

```sh
cmake --build build
python3 tests/vm_cycle_tests.py ./build/compiler_design vm-rs
cargo test --manifest-path vm-rs/Cargo.toml --test cycle_lifetime
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
python3 tests/debugger_tests.py ./build/compiler_design vm-rs
python3 tests/profile_tests.py ./build/compiler_design vm-rs
python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
```

## Next boundary

V1B is now the next VM slice. It compares retaining reference counting,
explicit weak links, cycle rejection, a non-moving tracing collector, and a
handle-based collector. The comparison must specify roots, native temporaries,
debugger observation, identity/hash/equality, final error state, pause points,
and embedding behavior before any V1C implementation work starts.
