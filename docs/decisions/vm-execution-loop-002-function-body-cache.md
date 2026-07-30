# VM-5B-002: lazy function-body cache

Status: implemented on 2026-07-30 on top of the trace-off instruction
preamble slice.

## Decision

`VM::call_function` now loads each referenced bytecode function body lazily by
function index. The first call copies the stable function name, parameter names,
register count, instructions, and debug locations into a
`CachedFunctionBody`; later calls reuse that immutable body through `Rc`.
Uncalled functions do not allocate a cached body. The cache belongs to one VM
instance and is released with it.

This removes the per-call cloning of the instruction and debug-location
vectors while retaining the existing per-frame register and parameter-cell
allocation. The main body remains a one-shot execution path and is not added to
the cache.

## Compatibility and non-goals

The cache does not change `.cdbc 0.1`, `Program`, function-value layout, output,
closures, recursion, aliasing, call-depth/resource limits, runtime errors,
source locations, trace events, debugger pauses, or profile counters. It does
not add eviction, cross-VM sharing, unsafe code, threaded dispatch, JIT, or a
new artifact version. Retaining an invoked body's copied vectors for the VM
lifetime is an intentional memory/performance tradeoff; capacity accounting and
eviction remain deferred to VM-5C.

## Evidence

The same two scaled workloads were run three times before and after the change
on the recorded host/toolchain. The cache produced the useful signal on the
call-heavy workload; the loop has no function-body calls and remained within
benchmark noise:

| Workload | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| `execution_closure` | 0.466308s | 0.441467s | -5.3% |
| `execution_loop` | 0.882778s | 0.933272s | +5.7% |

Both focused benchmark runs passed all correctness, stdout, stderr, and exit
status checks. The cache-specific Rust test also verifies that the body is
empty before the first call and that repeated calls share one allocation.
