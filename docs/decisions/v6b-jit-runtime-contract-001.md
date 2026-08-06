# V6B: JIT runtime contract

Status: contract recorded on 2026-08-06 after the V6A workload evidence
gate. This decision defines the compatibility boundary for a future baseline
JIT; it does not generate machine code, add an executable-memory dependency,
or change the `.cdbc 0.1` artifact.

## Context

The Rust VM has two execution paths that must keep one semantic source of
truth:

- the existing single-task path executes through the recursive VM loop;
- the V5B cooperative path owns explicit `ResumableFrame` and `FrameStack`
  state, FIFO scheduling, task roots, and dispatch-boundary collection.

Both paths already share bytecode instruction semantics, runtime helpers,
resource checkpoints, native dispatch, source locations, and typed runtime
errors. A JIT is admitted only as an optional tier inside those boundaries.

## Contract

### Compatibility and activation

1. The interpreter remains the default, compatibility, and rollback path.
   JIT execution is opt-in and may fall back for any unit or runtime boundary
   that is not supported.
2. V6B adds no CLI flag, public Rust API, artifact field, native name, or
   serialized machine-code pointer. A future opt-in must be explicit; an
   absent or disabled JIT configuration means interpreter execution.
3. Generated code belongs to one VM instance and one immutable `Program`.
   It is not shared between VMs, persisted in module caches, or embedded in
   `.cdbc 0.1`. Dropping the VM releases its code cache.
4. The initial baseline tier is non-speculative. It must not assume a
   language type, object layout, function identity, or native implementation
   beyond the verified bytecode and existing runtime checks.

### Compilation units and fallback

The first implementation may compile only verified entries in
`Program.functions`, with `execution_closure`'s two function bodies as the
first function-level workload candidate. The main body remains interpreter
controlled until a separate decision selects a synthetic-main representation.

Eligibility is an internal whitelist over verified bytecode. Unsupported
instructions, dynamic calls, callback-capable native boundaries, missing
metadata required by the active mode, or an unavailable code-cache budget
must select the interpreter before user-visible execution. They are fallback
conditions, not runtime errors. V6C must test both an eligible function and a
function that is rejected by the whitelist.

The baseline tier does not inline calls, specialize values, add inline
caches, or optimize across function/module boundaries. Guarded or optimizing
tiers require a later profile-backed decision with deoptimization metadata.

### Frame and register state

At every transfer into the interpreter, scheduler, native bridge, debugger,
error path, or GC safepoint, a compiled function must be able to materialize
the same logical state as the current frame:

- current function identity and `function_index`;
- bytecode-equivalent instruction position;
- all live register `Value`s;
- locals and closure environments;
- caller return register and call-site location; and
- the active task identity when running under a cooperative session.

The materialized form is the existing `Frame`/`ResumableFrame` contract, not
a second ownership model. JIT code must not retain raw pointers into `Rc`,
`RefCell`, register vectors, or source tables across a bridge or safepoint.
It must not hold a runtime borrow while returning to the scheduler or asking
the heap to collect.

### Checkpoints, scheduling, and roots

Compiled code observes the same checkpoint ordering as the interpreter:

- instruction steps use the shared `checkpoint_instruction` semantics,
  including cancellation-before-limit ordering and checked overflow;
- native loops use the shared `checkpoint_native` semantics;
- runtime-element and output charging remains in the existing VM helpers;
- a quantum expires only at a scheduler-admitted boundary; and
- cancellation, resource failure, return, and debugger quit materialize a
  frame before control leaves the current task.

The JIT does not own scheduling and never starts an OS thread. A cooperative
task either executes through a JIT path that can return at the required
quantum/checkpoint boundary or uses the interpreter fallback. The initial V6C
baseline may keep cooperative sessions interpreter-only while this boundary
is implemented; that fallback is explicit and preserves the V5 contract.

GC remains the existing non-moving, stop-the-world collector. At every
admitted collection boundary, roots include all live materialized frames,
register values, locals, closures, globals, scheduler-owned task payloads,
native temporaries, returned values, and debugger observations. JIT code may
not expose machine addresses as roots and may not run collection concurrently
with an active runtime borrow.

### Native calls and callbacks

Compiled code never calls a native function pointer directly. Native calls
cross a VM-owned bridge that retains the existing native registry, arity and
type diagnostics, resource charging, cancellation checks, output accounting,
call-depth accounting, and source call-site.

Callback-capable natives (`map`, `filter`, `flatMap`, `any`, `all`, `count`,
`find`, `findIndex`, and `reduce`) are mandatory frame/VM transitions. The
baseline tier may conservatively fall back before such an instruction. If a
later tier admits one, it must materialize the caller frame, preserve the
active task identity, allow callback execution through the existing function
path, and resume with the same return-register and error-stack contract.

### Observability and diagnostics

The interpreter remains authoritative whenever exact existing observability
is requested. In particular, V6C initially falls back to the interpreter for
single-task trace, profile, debugger, and cooperative task-aware sessions.
This keeps the current event order, per-bytecode profile counters, source
range hits, pause locals, task queue observations, and output sequences
unchanged while the uninstrumented baseline tier is validated.

If a later slice enables JIT under an observable mode, it must either emit
the exact bytecode-equivalent records or prove an explicit compatibility
mapping. It may not silently omit instructions, merge source ranges, reorder
output, or change profile totals.

Runtime failures from compiled code use the same `RuntimeError` fields as the
interpreter: kind, configured resource limit, source location, source table,
and call-site stack. A failure at a compiled instruction must report the
bytecode-equivalent location and preserve the existing no-rollback mutation
semantics. If the required mapping cannot be reconstructed, execution falls
back before entering the compiled unit.

### Code-cache policy and invalidation

The future opt-in configuration must carry a finite per-VM code-cache budget.
There is no unlimited executable-code default. Compilation is refused when
the next unit would exceed the budget, and the interpreter continues to run.
Code-cache accounting is separate from `.cdbc` artifact and runtime-element
budgets; it must not change existing resource errors when JIT is disabled.

The baseline tier compiles immutable bytecode without speculative guards, so
there is no deoptimization protocol in V6C. A cache entry is invalidated by
VM/program lifetime or explicit cache eviction only. Self-modifying bytecode,
cross-VM cache sharing, persistent native pointers, and tiered deoptimization
are outside this contract and require a later decision.

## Implementation gate for V6C

Before any machine-code implementation is admitted, the slice must provide:

1. an explicit internal eligibility result and interpreter fallback reason;
2. a bounded per-VM code-cache owner with no artifact serialization;
3. frame materialization tests at normal return, call/return, cancellation,
   resource failure, and GC boundaries;
4. native and callback fallback tests, including call-depth and cancellation;
5. byte-for-byte output, typed-error, location, and exit parity for the V6A
   workloads; and
6. explicit tests proving JIT-off behavior and unsupported-unit fallback are
   identical to the current interpreter.

The first machine-code slice should target only the measured function-level
candidate from `execution_closure`, use a small non-speculative instruction
subset, and retain the interpreter for every other unit and observable mode.
`execution_loop` remains an end-to-end control workload until a function-level
hot body is identified rather than treating the main chunk as an implicit
`BytecodeFunction`.

## Non-goals

This contract does not select a native-code backend, assembler, platform
matrix, calling convention, executable-memory allocator, optimizing tier,
inline cache, deoptimizer, binary artifact format, OS-thread scheduler, or
language-level async feature. It also does not authorize changing O0/O1
compiler behavior or making JIT execution the default.

## Verification boundary

This decision adds no production code or artifact bytes. The contract is
reviewed with `git diff --check` and link checks. V6C must run the relevant
Rust VM, artifact, malformed, debugger, profile, cooperative, golden, and
parity gates before claiming an implementation.
