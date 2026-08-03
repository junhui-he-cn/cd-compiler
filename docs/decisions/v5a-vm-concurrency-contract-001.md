# V5A: deterministic cooperative concurrency contract

Status: resolved on 2026-08-03 against the Rust VM baseline at `abd04543`.
This is a contract-only slice. V5B scheduler implementation, task-aware API
types, and source-level concurrency syntax are not included.

## Consumer and boundary

The named consumer is an in-process Rust library host that runs a bounded set
of cooperative bytecode tasks in one VM session and consumes typed task
completion, waiting, cancellation, and failure state. The CLI remains a
single-program adapter. V5A does not invent a language-level task API or a
process-boundary result schema; V2 host outcome and controlled I/O remain
separate deferred tracks.

The scheduler is cooperative and deterministic on one OS thread. It is not an
OS-thread pool, an async executor, or a `Send`/`Sync` boundary. `.cdbc 0.1`,
existing single-task `VM::run`, `trace`, `debug`, and `profile` behavior, and
the current C++/Rust single-task parity contract remain unchanged.

## Decision summary

1. A scheduler owns immutable verified bytecode plus explicit resumable task
   state. A task can stop only at an instruction/native checkpoint, explicit
   scheduler yield, blocked wait, terminal return, failure, or cancellation.
2. Ready tasks run in FIFO creation/requeue order. One task runs at a time;
   a dispatch quantum is an instruction-count fairness limit, not a resource
   budget. Reaching the quantum requeues the task at the tail.
3. Task-local frames, registers, locals, and closure environments are private
   roots. The scheduler owns suspended tasks and keeps their roots visible to
   GC. Existing `Value` handles remain shallow and identity-preserving when a
   host explicitly shares a value with a task.
4. Scheduler-wide resource budgets preserve the current accounting model.
   Instruction steps, runtime elements, output bytes, and cancellation are
   checked across the whole session; call depth is checked per task.
5. Output and observable events use one scheduler sequence in dispatch order.
   Debugger pause stops the scheduler, not just an invisible background task.
   No streaming sink or wall-clock scheduling is added by this contract.
6. A task failure is retained as typed state and deterministically stops new
   user execution under the default fail-fast policy. Joiners observe the
   target outcome; unjoined failures are not silently detached.
7. JIT code, when admitted by V6, runs under the same scheduler order,
   checkpoint, cancellation, resource, and root rules. The interpreter stays
   the fallback and compatibility path.

## Task lifecycle and scheduling

The scheduler exposes the following internal lifecycle states:

| State | Transition rule |
| --- | --- |
| `Ready` | Eligible for the next FIFO dispatch. |
| `Running` | Owns the single execution turn; at most one task has this state. |
| `Blocked` | Waiting for a scheduler-defined join or wake event; it is not dispatched. |
| `Completed` | Returned a value and can satisfy joiners. |
| `Failed` | Stored a `RuntimeError` and can satisfy joiners. |
| `Cancelled` | Observed cancellation at a checkpoint or was cancelled while blocked. |

Task IDs are monotonically assigned by one scheduler instance. They are
diagnostic identities only and must not enter `.cdbc`, persisted artifacts, or
cross-VM handles.

Each dispatch runs until the task reaches a terminal state, blocks, yields, or
consumes its positive quantum. A native callback is synchronous within its
dispatch; the scheduler does not interrupt arbitrary Rust code. Native loops
continue to use the existing instruction/resource checkpoints. A task that
uses its quantum without another state transition returns to `Ready`.

Spawn and requeue order are deterministic. A newly spawned task is appended to
the ready queue after tasks already queued for the current scheduling point.
When no task is ready but one or more tasks are blocked, the scheduler returns
a typed waiting state to the host rather than using time, polling, or an
implicit OS wakeup. V5B may initially admit only join-based blocking and
explicit host wake events; filesystem, network, clock, and other external I/O
remain outside this contract.

Joining a running task blocks the joining task until the target is terminal.
Join waiters wake in registration order. Joining a task by itself is a
deterministic runtime error. A successful join returns the target value; a
failed or cancelled target propagates that typed outcome to the joiner.

## Cancellation and resource accounting

The existing `CancellationToken` remains the scheduler-wide cancellation
request. Once observed, the running task stops at the next admitted checkpoint,
blocked tasks become cancelled, and new tasks do not begin user execution. A
future task-specific cancellation request follows the same checkpoint rule and
does not interrupt native Rust code synchronously.

The scheduler preserves these budget scopes:

| Budget | Scope in V5B |
| --- | --- |
| Instruction steps | One cumulative counter across all task dispatches and native checkpoints. |
| Call depth | One active-frame count per task. |
| Runtime elements | One cumulative VM/heap counter shared by the scheduler. |
| Output bytes | One cumulative output buffer counter shared by the scheduler. |
| Artifact bytes/modules | Validated before task execution, unchanged from the current linker boundary. |

The per-task quantum never disables or resets the global instruction budget.
Budget, runtime, or cancellation failure follows the current no-rollback rule:
completed mutations remain applied internally, while the existing `run`-style
successful output result is not returned after an overall failure.

## Ownership, roots, and collection

The scheduler owns one execution heap and the immutable program. Task frames
and task-local environments are separate, while explicitly shared closures,
cells, arrays, maps, and structs retain the current shallow alias semantics.
There is no implicit deep copy, cross-thread value transfer, or `Send`/`Sync`
promise. A host that needs isolation creates separate scheduler/VM instances;
V5B does not add a new language-level shared-memory primitive.

At every admitted scheduler safepoint, roots include:

- the running task's frames, registers, locals, and closure environments;
- every suspended task's equivalent frame and wait state;
- scheduler-owned globals, task handles, join payloads, and pending results;
- native temporaries and values being assembled at the current boundary; and
- debugger-held observations and host-held values admitted by the future typed
  API.

Collection remains stop-the-world on the same OS thread. It may run only after
a task has returned to the scheduler with no active runtime borrow. The V1
top-level collection boundary remains valid for single-task APIs; V5B adds
dispatch-boundary collection only after the suspended-task root set is
implemented and tested. No background or concurrent collector is implied.

## Errors, output, and observability

The default scheduler policy is fail-fast: the first task failure, resource
failure, or cancellation in deterministic dispatch order stops new user
instructions and transitions remaining nonterminal tasks to cancellation or
cleanup. The first failure remains the scheduler result. A future structured
host result may expose per-task terminal records, but V5A does not define the
V2 external schema.

Output is appended in the order instructions commit it. Trace and profile
records receive one monotonic scheduler sequence, with task identity in the
task-aware API; existing single-task record shapes remain unchanged. A debug
pause captures the selected task and scheduler queue state and prevents other
tasks from running until the host resumes or quits. Repeated runs with the
same artifact, configuration, and host wake sequence must produce identical
output, terminal outcomes, trace ordering, profile counters, and resource
errors.

## V5B entry gate

V5B may begin only with evidence for:

- explicit resumable frame stacks rather than relying on Rust call recursion;
- FIFO quantum scheduling, spawn order, yield, join, blocked/wake, and
  no-ready-task waiting behavior;
- global budget accounting and cancellation across multiple tasks;
- per-task roots, rooted/unrooted cycles, and collection at dispatch
  safepoints;
- deterministic output, trace, profile, debugger, native callback, and
  runtime-error ordering; and
- shallow aliasing and task cleanup without changing existing single-task
  parity or `.cdbc 0.1` bytes.

The first implementation should remain host-only and one-threaded. Async
syntax, channels/actors, shared-memory language semantics, OS threads,
`Send`/`Sync`, preemptive timing, and JIT code generation require later
decisions.

## Rejected alternatives

| Alternative | Reason |
| --- | --- |
| OS-thread-per-task execution | Adds nondeterministic ordering, cross-thread roots, and `Send`/`Sync` obligations before a consumer requires them. |
| Wall-clock or timer preemption | Makes output, cancellation, and resource observations host-dependent. |
| Implicit deep-copy task isolation | Breaks current identity and closure-cell alias semantics. |
| Detached task failures | Hides runtime errors and makes a scheduler result depend on host cleanup timing. |
| JIT before V5B | Fixes frame, root, cancellation, and safepoint assumptions before the scheduler contract exists. |

## Verification boundary

This decision adds no production code, artifact bytes, CLI options, or public
schema. Its verification is document-level (`git diff --check` and link review);
the executable evidence listed under the V5B entry gate is required when the
implementation slice starts.
