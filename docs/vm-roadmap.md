# Compiler Design VM Roadmap

This is the active plan for the Rust VM in `vm-rs/`. The compiler, language,
IR, module-cache producer, and compiler tools are planned in
[`docs/roadmap.md`](roadmap.md). Changes to `.cdbc`, native calls, module
products, debug metadata, or compatibility policy are joint slices.

The baseline was audited on 2026-08-02 at `master` commit `dcbfedf8`. Detailed
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
| Runtime values | Shared cells and identity-bearing arrays, maps, structs, closures, recursive values, cycle-safe formatting | Reference-counted cycles are observable but not reclaimed |
| Modules | Deterministic module validation/linking, debug rebasing, typed errors, optional link report | Versioned report serialization is not defined |
| Embedding | Rust library parse/verify/link/run/trace/debug/profile API plus CLI adapters | API remains pre-1.0, single-threaded, and without host sink/session guarantees |
| Observability | Interactive debugger, deterministic counters, tracked heap counts, estimated retained bytes, structured error kinds | No stable host schema, wall-clock field, allocator bytes, or RSS contract |
| Performance/capacity | Reproducible phase benchmark, scaled workloads, capacity and budget corpus, 29 measured execution-loop slices | The micro-optimization queue is closed pending new profile evidence |
| Native boundary | Private registry with arity, callback, resource-touchpoint, and signature-shape metadata | Metadata is not a public ABI or serialized contract |

`cdbc 0.1`, CLI text/exit behavior, deterministic execution, current resource
accounting, and C++/Rust parity remain compatibility constraints.

## 3. Joint prerequisite

### X1: Compiler/VM compatibility matrix

**Status:** active. This is shared with `docs/roadmap.md` and is the next VM
slice.

Record compiler, artifact, module product, debug metadata, module-cache,
native-name, VM library, and CLI compatibility in one test-backed matrix.
Cover current success, metadata-free compatibility, and unsupported-version
rejection. Do not change artifact bytes or versions in this slice.

## 4. VM queue

### V1: Resolve recursive-object lifetime and cycle policy

**Priority:** P0. **Status:** queued after X1.

Recursive named structs make cycles a supported source-level construction, and
the VM already formats active cycles as `<cycle>`. The remaining lifetime
behavior can no longer stay as an incidental `Rc<RefCell<...>>` property.

Proceed in three slices:

1. **V1A - cycle corpus and measurement:** add source-backed self-cycle,
   mutual-cycle, array/map/struct cycle, closure/environment cycle, cycle
   replacement, runtime-error, debugger/profile, and repeated in-process VM
   workloads. Measure tracked live objects and estimated retained bytes before
   and after roots and VM instances are dropped. Host RSS is observational,
   not a portable pass threshold.
2. **V1B - storage decision:** compare retaining reference counting, explicit
   weak links, cycle detection/rejection, a non-moving tracing collector, and
   a handle-based collector. Specify roots, native temporaries, debugger
   observation, identity/hash/equality, final error state, pause points, and
   embedding behavior.
3. **V1C - admitted implementation:** implement only the selected policy with
   an incremental migration and rollback path. Keep `cdbc 0.1` unchanged unless
   the decision proves an artifact-visible change is unavoidable.

**Decision gate:** do not introduce GC, weak-reference syntax, relocating
handles, or cycle rejection automatically. Stop after V1B evidence for the
policy choice.

**Gate:** C++/Rust recursive-value parity, alias and identity cases, cycle-safe
formatting, library multi-instance tests, runtime failure and cancellation,
trace/debug/profile determinism, resource limits, capacity corpus, and Cargo
tests.

### V2: Define a real host integration boundary

**Priority:** P1. **Status:** queued, but requires a concrete in-repository host
consumer before API commitment.

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

**Priority:** P1. **Status:** queued after X1 and independent of V1. Later
hotspot work still requires its own profile evidence.

The previous execution-loop micro-slice sequence is closed. New work starts
only from a reproducible profile or capacity result:

1. **V3A - isolate artifact load:** separate parse/verify load measurement from
   canonical `dump` formatting in the benchmark runner.
2. **V3B - select one hotspot:** name the workload, allocation/instruction
   mechanism, expected metric, correctness boundary, and same-sequence
   baseline/candidate runs before editing the VM.
3. **V3C - capacity policy:** add a capacity case only when it defines a new
   exact success/rejection boundary or exposes an unbounded host cost.

Timing remains evidence rather than a cross-platform correctness gate. Every
candidate must preserve stdout, stderr, exit code, trace/debug/profile events,
resource accounting, and linked/module artifact behavior.

### V4: Stabilize native and release compatibility

**Priority:** P2. **Status:** queued behind X1 and a real host or artifact need.

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
X1 compatibility matrix
  -> V1A cycle corpus and measurement
  -> V1B lifetime/storage decision
  -> V1C selected implementation

real host consumer + X1
  -> V2A structured host outcome
  -> V2B controlled I/O
  -> optional V2C external schema

existing benchmark/profile/capacity baseline
  -> V3A pure load measurement
  -> V3B one evidence-selected hotspot
  -> V3C exact capacity policy as needed

X1 + demonstrated ABI/release need
  -> V4 native/release compatibility decision
  -> optional successor artifact work
```

The recommended next VM slice after X1 is V1A. V2 must wait for a concrete
consumer; V3 may proceed independently only from recorded evidence.

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
