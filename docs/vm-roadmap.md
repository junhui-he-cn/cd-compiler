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
| Embedding | Rust library parse/verify/link/run/trace/debug/profile API plus CLI adapters | Host outcome/sink/session work is explicitly deferred; API remains pre-1.0 and single-threaded |
| Observability | Interactive debugger, deterministic counters, tracked heap counts, estimated retained bytes, structured error kinds | No stable host schema, wall-clock field, allocator bytes, or RSS contract |
| Performance/capacity | Reproducible phase benchmark, scaled workloads, capacity and budget corpus, artifact-load and format-capacity evidence | Broader host-cost capacity policy remains deferred |
| Native boundary | Private registry with arity, callback, resource-touchpoint, and signature-shape metadata | Metadata is not a public ABI or serialized contract |

`cdbc 0.1`, CLI text/exit behavior, deterministic execution, current resource
accounting, and C++/Rust parity remain compatibility constraints.

## 3. Active VM queue

The X1 compatibility matrix, V1 recursive-object lifetime policy, and V3
profile-driven performance slices are complete and intentionally omitted from
this active queue. Their decision records and verification evidence remain in
`docs/decisions/` and Git history.

The next execution plan is deterministic concurrency first, followed by the
evidence-driven JIT. V5 establishes task, frame, root, safepoint, cancellation,
and observable-ordering semantics; V6 consumes that contract. V2 host
integration and V4 native/release compatibility remain trigger-based deferred
tracks and do not block this sequence.

### V2: Define a real host integration boundary

**Priority:** trigger-based. **Status:** explicitly deferred on 2026-08-03. The consumer
gate was audited and a concrete in-repository host consumer is still required
before API commitment. See
[`v2-host-consumer-gate-001.md`](decisions/v2-host-consumer-gate-001.md) and
[`vm-future-work-deferred-001.md`](decisions/vm-future-work-deferred-001.md).

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

### V4: Stabilize native and release compatibility

**Priority:** trigger-based. **Status:** explicitly deferred on 2026-08-03, pending a real
ABI or release need. See
[`vm-future-work-deferred-001.md`](decisions/vm-future-work-deferred-001.md).

- Publish native registry metadata only after deciding whether names remain the
  artifact ABI or a successor format introduces versioned native IDs.
- Centralize runtime type validation only when it preserves existing
  native-specific diagnostics and callback/resource ordering.
- Define a release matrix for compiler version, VM library version, CLI
  version, `cdbc`, module products, and debug metadata before any public crate
  or successor artifact release.

Private metadata is useful implementation structure, but it is not permission
to expose a plugin ABI.

### V5: Define deterministic task scheduling before parallel execution

**Priority:** P1. **Status:** V5A contract recorded on 2026-08-03; the V5B
scheduler control-plane and explicit frame-state foundation are implemented;
the task execution adapter is next. See
[`v5a-vm-concurrency-contract-001.md`](decisions/v5a-vm-concurrency-contract-001.md).

Concurrency is a runtime and language contract that a later JIT must preserve.
It determines task-local frames and roots, GC safepoints, cancellation, output
and failure ordering, resource charging, native callback behavior, and
debugger/profile events. Resume it before JIT work, in these slices:

1. **V5A - concurrency contract:** recorded in
   [`v5a-vm-concurrency-contract-001.md`](decisions/v5a-vm-concurrency-contract-001.md).
   The named consumer is an in-process Rust library host; the contract fixes
   one-thread cooperative scheduling, task lifecycle, join/wake, cancellation,
   resource scopes, output/event ordering, and task-root ownership. It does not
   promise OS threads, language-level async syntax, or `Send`/`Sync`.
2. **V5B - cooperative scheduler:** the control-plane foundation and private
   one-task execution adapter are recorded in
   [`v5b-vm-scheduler-control-plane-001.md`](decisions/v5b-vm-scheduler-control-plane-001.md).
   It provides FIFO task states, quantum requeue, explicit wake,
   cancellation transitions, a resumable frame stack with validated return
   transfer, dispatch-boundary GC, and parity checks for output, budgets,
   callbacks, roots, and runtime-error stacks. The adapter remains host-only
   and private; source syntax, `.cdbc 0.1`, and existing CLI/library behavior
   are unchanged. The next slice is the multi-task host result and join/wake
   integration required by the V5A contract.
3. **V5C - concurrency expansion decision:** only after V5B workloads exist,
   decide whether async syntax, channels/actors, shared-memory concurrency,
   multiple OS threads, or `Send`/`Sync` provides enough product value to
   justify its nondeterminism and synchronization costs.

**Gate:** repeated-run determinism, task lifecycle and cancellation, per-task
root tracing and cycle collection, native callbacks, debugger pauses,
trace/profile event order, resource limits, runtime failures, and CLI/library
parity.

### V6: Add an evidence-driven JIT after the scheduling contract

**Priority:** P2. **Status:** planned immediately after V5B and its evidence
gate.

JIT compilation is an optional execution optimization, not a new language
semantic. Starting it after V5B prevents a single-thread-only code generator
from fixing assumptions about frames, roots, safepoints, cancellation, and
thread state that concurrency would later invalidate.

1. **V6A - hot-workload evidence:** use stable profiles and reproducible
   benchmarks to identify an eligible `BytecodeFunction` subset and a concrete
   performance target. Do not add native code generation for synthetic
   microbenchmarks alone.
2. **V6B - JIT runtime contract:** specify interpreter fallback, source/debug
   and runtime-error mapping, GC root maps and safepoints, cancellation and
   resource checks, executable code-cache limits, native-call transitions,
   and invalidation/deoptimization behavior where guards are required. Keep
   `.cdbc` independent from generated machine code.
3. **V6C - optional baseline JIT:** compile only the measured hot function
   subset, retain deterministic interpreter fallback and an off switch, and
   verify identical output, failures, debug locations, observable profile
   semantics, and resource-limit behavior. Tiering or optimizing JIT work
   requires a later profile-backed decision.

**Decision gate:** do not place JIT implementation in the default VM queue
before V5B establishes the task, frame, root, and safepoint model. A later JIT
slice must preserve the interpreter as the compatibility and rollback path.

## 4. Deferred VM work

These tracks stay outside the default queue until their trigger is met:

- binary or successor artifacts: require measured load/size/integrity pressure
  and the existing compatibility matrix;
- persistent sessions, snapshot, or rollback: require a host/REPL consumer and
  the established lifetime policy;
- `Send`/`Sync`, async scheduling, or concurrency expansion beyond V5: follow
  V5C and require a concrete consumer plus language and deterministic
  scheduling decisions;
- optimizing JIT tiers or native code generation beyond V6: follow the V6
  contract and require stable profiles, a fallback interpreter, debug/error
  mapping, GC safepoints, code-cache limits, and demonstrated hot workloads;
- exact allocator/RSS reporting: require a platform/tooling policy distinct
  from VM-owned retained-byte estimates;
- filesystem, network, time, randomness, and dynamic native plugins: require
  explicit capabilities, test doubles, resource budgets, and deterministic
  fallback behavior.

## 5. Dependency order

```text
V5A concurrency contract (recorded)
  -> V5B scheduler control plane (implemented)
  -> V5B resumable frame foundation (implemented)
  -> V5B task execution adapter
  -> optional V5C concurrency expansion

completed V5B + stable profile + demonstrated hot workload
  -> V6A hot-workload evidence
  -> V6B JIT runtime contract
  -> optional V6C baseline JIT

real host consumer
  -> V2A structured host outcome
  -> V2B controlled I/O
  -> optional V2C external schema

demonstrated ABI/release need
  -> V4 native/release compatibility decision
  -> optional successor artifact work
```

V5B task execution adapter is the next implementation focus. V6 JIT work starts
only after V5B establishes deterministic task/frame/root/safepoint behavior and
stable hot-workload evidence. V2 host integration and V4 release compatibility
resume only when their named consumer or ABI/release trigger appears.

## 6. Verification contract

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

## 7. Completion rule

VM progress is measured by closed runtime contracts and reproducible evidence,
not by the count of interpreter micro-optimizations. A storage, host, native,
or artifact change is complete only when its lifecycle, compatibility,
diagnostics, observability, and rollback boundary are all explicit.
