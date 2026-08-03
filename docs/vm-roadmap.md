# Compiler Design VM Roadmap

This is the active plan for the Rust VM in `vm-rs/`. The compiler, language,
IR, module-cache producer, and compiler tools are planned in
[`docs/roadmap.md`](roadmap.md). Changes to `.cdbc`, native calls, module
products, debug metadata, or compatibility policy are joint slices.

The baseline was audited on 2026-08-02 at `master` commit `a78cce37`. Detailed
history lives in `docs/decisions/`, tests, and Git; completed micro-slices are
not repeated here.

## 1. VM product boundary

The VM is a deterministic, validated, observable `.cdbc` execution engine. It
must:

- reject malformed, unsupported, over-budget, or unlinkable products before
  unsafe execution state is observed;
- preserve compiler-defined value, alias, call, native, output, and error
  semantics;
- expose equivalent CLI and Rust library behavior;
- keep ordinary execution independent from opt-in trace, debug, profile, and
  capacity instrumentation; and
- provide evidence before changing storage, performance, or artifact policy.

The VM does not independently invent language syntax, type semantics, module
cache invalidation, async semantics, or compiler optimization policy.

## 2. Current baseline

| Area | Shipped baseline | Open boundary |
| --- | --- | --- |
| Artifact safety | Shared `cdbc 0.1` parser/formatter/verifier, malformed corpus, resource limits, cancellation | Successor format and integrity envelope are not justified |
| Execution | Register VM for the complete emitted instruction set and native surface | Further optimization must be workload-driven |
| Runtime values | Stable identity-bearing storage, non-moving tracing collection at VM safepoints, recursive values, cycle-safe formatting | Incremental/concurrent scheduling remains open; frequency evidence does not justify a threshold |
| Modules | Deterministic module validation/linking, debug rebasing, typed errors, optional link report | Versioned report serialization is not defined |
| Embedding | Rust library parse/verify/link/run/trace/debug/profile API plus CLI adapters | API remains pre-1.0, single-threaded, and without host sink/session guarantees |
| Observability | Interactive debugger, deterministic counters, tracked heap counts, estimated retained bytes, structured error kinds | No stable host schema, wall-clock field, allocator bytes, or RSS contract |
| Performance/capacity | Reproducible phase benchmark, scaled workloads, capacity and budget corpus, V3A/V3B/V3C artifact evidence | Broader host-cost capacity policy remains deferred |
| Native boundary | Private registry with arity, callback, resource-touchpoint, and signature-shape metadata | Metadata is not a public ABI or serialized contract |

`cdbc 0.1`, CLI text/exit behavior, deterministic execution, current resource
accounting, and C++/Rust parity remain compatibility constraints.

## 3. Joint prerequisite

### X1: Compiler/VM compatibility matrix

**Status:** completed on 2026-08-02. This is shared with
[`x1-compiler-vm-compatibility-001.md`](decisions/x1-compiler-vm-compatibility-001.md).

The shipped matrix records compiler, artifact, module product, debug metadata,
module-cache, native-name, VM library, and CLI compatibility in one test-backed
matrix. It validates seven cells against source constants, the native registry,
inventory revision, and evidence paths. The refreshed artifact audit covers 126
assertions across 63 fixtures. No artifact bytes or versions changed.

## 4. VM queue

### V1: Resolve recursive-object lifetime and cycle policy

**Priority:** P0. **Status:** active; V1A completed, V1B resolved, and the
first V1C implementation slice completed on 2026-08-02.

Recursive named structs make cycles a supported source-level construction, and
the VM already formats active cycles as `<cycle>`. The remaining lifetime
behavior can no longer stay as an incidental `Rc<RefCell<...>>` property.

Proceed in three slices:

1. **V1A - cycle corpus and measurement:** completed with decision record
   [`v1a-vm-cycle-corpus-001.md`](decisions/v1a-vm-cycle-corpus-001.md). The
   source-backed self-cycle,
   mutual-cycle, array/map/struct cycle, closure/environment cycle, cycle
   replacement, runtime-error, debugger/profile, and repeated in-process VM
   workloads. Measure tracked live objects and estimated retained bytes before
   and after roots and VM instances are dropped. Host RSS is observational,
   not a portable pass threshold.
2. **V1B - storage decision:** resolved with
   [`v1b-vm-lifetime-storage-001.md`](decisions/v1b-vm-lifetime-storage-001.md).
   The selected policy is a non-moving tracing collector over stable tracked
   storage; roots, native temporaries, debugger observation, identity,
   diagnostics, pause points, and embedding behavior are specified there.
3. **V1C - admitted implementation:** first slice complete. `Heap` now exposes
   explicit non-moving tracing collection and VM execution collects after the
   top-level frame, with coverage for native callbacks, nested calls,
   cancellation, debugger pauses, host-held roots, and recursive variant
   payloads. The frequency/resource comparison is recorded in
   [`v1c-vm-tracing-frequency-001.md`](decisions/v1c-vm-tracing-frequency-001.md);
   it retains the top-level safepoint and does not justify an allocation
   threshold or background work. Keep `cdbc 0.1` unchanged and retain an
   incremental rollback path.

**Decision gate:** V1B selects tracing GC. Do not add weak-reference syntax,
relocating handles, or cycle rejection; stop V1C before changing storage layout
until root, pause, and resource evidence is complete.

**Gate:** C++/Rust recursive-value parity, alias and identity cases, cycle-safe
formatting, library multi-instance tests, runtime failure and cancellation,
trace/debug/profile determinism, resource limits, capacity corpus, and Cargo
tests.

### V2: Define a real host integration boundary

**Priority:** P1. **Status:** consumer gate audited on 2026-08-03 and remains
queued; a concrete in-repository host consumer is still required before API
commitment. See [`v2-host-consumer-gate-001.md`](decisions/v2-host-consumer-gate-001.md).

1. **V2A - host outcome:** define one structured execution outcome containing
   output, typed runtime/resource failure, frames/source ranges, and partial
   profile data without parsing CLI text.
2. **V2B - controlled I/O:** add output and diagnostic sinks only with explicit
   reentrancy, cancellation, resource charging, failure, and determinism rules.
3. **V2C - schema decision:** version a JSON or other external schema only when
   a consumer needs a process boundary; keep Rust typed APIs authoritative
   otherwise.

No filesystem, network, clock, randomness, dynamic plugin, persistent-session,
or `Send`/`Sync` promise belongs in V2A.

### V3: Replace micro-optimization with profile-driven performance work

**Priority:** P1. **Status:** V3A-V3C complete; later hotspot or capacity work
still requires its own profile or exact-boundary evidence.

The previous execution-loop micro-slice sequence is closed. New work starts
only from a reproducible profile or capacity result:

1. **V3A - isolate artifact load:** completed on 2026-08-03 with
   [`v3a-vm-artifact-load-001.md`](decisions/v3a-vm-artifact-load-001.md).
   The benchmark runner now measures a no-output `verify` load phase
   (read/parse/verify) separately from the canonical `dump` phase, while
   preserving the existing artifact bytes and output contract.
2. **V3B - select one hotspot:** completed on 2026-08-03 with
   [`v3b-vm-format-capacity-001.md`](decisions/v3b-vm-format-capacity-001.md).
   The selected hotspot is canonical formatting of multi-megabyte artifacts;
   a conservative output-capacity hint reduces formatter buffer growth while
   preserving byte-for-byte dumps and all runtime behavior.
3. **V3C - capacity policy:** completed on 2026-08-03 with
   [`v3c-vm-artifact-budget-boundary-001.md`](decisions/v3c-vm-artifact-budget-boundary-001.md).
   The new `verify` command now has an exact artifact-byte success/rejection
   boundary, paired with `dump`, without changing the default limit policy.

Timing remains evidence rather than a cross-platform correctness gate. Every
candidate must preserve stdout, stderr, exit code, trace/debug/profile events,
resource accounting, and linked/module artifact behavior.

### V4: Stabilize native and release compatibility

**Priority:** P2. **Status:** queued behind completed X1 and a real host or artifact need.

- Publish native registry metadata only after deciding whether names remain the
  artifact ABI or a successor format introduces versioned native IDs.
- Centralize runtime type validation only when it preserves existing
  native-specific diagnostics and callback/resource ordering.
- Define a release matrix for compiler version, VM library version, CLI
  version, `cdbc`, module products, and debug metadata before any public crate
  or successor artifact release.

Private metadata is useful implementation structure, but it is not permission
to expose a plugin ABI.

## 5. Deferred VM work

These tracks stay outside the default queue until their trigger is met:

- binary or successor artifacts: require measured load/size/integrity pressure
  and the X1 compatibility matrix;
- persistent sessions, snapshot, or rollback: require a host/REPL consumer and
  a completed V1 lifetime policy;
- `Send`/`Sync`, tasks, or async scheduling: require language and deterministic
  scheduling decisions;
- JIT or native code generation: require stable profiles, a fallback
  interpreter, debug/error mapping, code-cache limits, and demonstrated hot
  workloads;
- exact allocator/RSS reporting: require a platform/tooling policy distinct
  from VM-owned retained-byte estimates;
- filesystem, network, time, randomness, and dynamic native plugins: require
  explicit capabilities, test doubles, resource budgets, and deterministic
  fallback behavior.

## 6. Dependency order

```text
completed X1 compatibility matrix
  -> completed V1A cycle corpus and measurement
  -> completed V1B lifetime/storage decision
  -> completed V1C top-level tracing slice

real host consumer + X1
  -> V2A structured host outcome
  -> V2B controlled I/O
  -> optional V2C external schema

existing benchmark/profile/capacity baseline
  -> completed V3A pure load measurement
  -> completed V3B one evidence-selected hotspot
  -> completed V3C exact artifact-byte boundary

completed X1 + demonstrated ABI/release need
  -> V4 native/release compatibility decision
  -> optional successor artifact work
```

X1, V1A, V1B, the first V1C implementation slice, and its frequency/resource
measurement are complete. Incremental/concurrent scheduling remains
evidence-gated. V2 must wait for a concrete consumer; V3 may proceed
independently only from recorded evidence.

## 7. Verification contract

For a Rust VM-only slice, run at least:

```sh
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
python3 tests/debugger_tests.py ./build/compiler_design vm-rs
python3 tests/profile_tests.py ./build/compiler_design vm-rs
python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
git diff --check
```

Changes to the parser, verifier, linker, native names, resource accounting,
module products, debug metadata, or compiler emitter also require the affected
module-cache, malformed, boundary, golden, CTest, and canonical verification
commands from `AGENTS.md` and `docs/roadmap.md`.

Performance work must record baseline and candidate commit, host/toolchain,
workload digest, repetition count, same-sequence measurements, and exact
output/error/exit parity. Generated reports are evidence artifacts, not source
files to commit unless a decision explicitly defines a checked-in baseline.

## 8. Completion rule

VM progress is measured by closed runtime contracts and reproducible evidence,
not by the count of interpreter micro-optimizations. A storage, host, native,
or artifact change is complete only when its lifecycle, compatibility,
diagnostics, observability, and rollback boundary are all explicit.
