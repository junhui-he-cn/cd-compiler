# C1B-REG-001: O1 virtual-register pressure policy

Status: implemented on 2026-08-03 on `feat/next-development-20260803`.

## Question

Does the explicit O1 pipeline need a physical register allocation/coalescing
stage now, or can it retain the existing virtual-register IR while the C1
evidence is collected?

## Decision

Keep O1 on the existing virtual-register path. Do not add physical register
allocation, coalescing, or spilling to O1 or the default O0 path in this
slice.

The benchmark runner now records `peak_live_virtual_registers` for every IR
body. It computes liveness over the printed control-flow edges, joins both
successors of conditional branches, and iterates loops to a fixed point. The
report exposes the maximum across all bodies plus separate main/function
maxima. The existing virtual-register count and bytecode `registerCount`
remain separate metrics: the former describes the virtual numbering shape and
the latter describes the emitted register-file slots.

Physical allocation remains deferred until a later decision writes down:

- the virtual-to-physical mapping and frame/register ownership;
- spill and reload behavior at calls, aggregate effects, callbacks, and
  module boundaries; and
- the mapping from optimized source locations, frame locals, and runtime
  cells to debugger-visible values.

## Evidence

The checked-in benchmark matrix was measured at both optimization levels with
three repetitions per workload:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py \
  ./build/compiler_design vm-rs \
  --compare-opt-levels \
  --report build/benchmark-report-o0-o1-c1b.json
```

Result: `14 passed, 0 failed`, with O0/O1 output, error, exit, and Rust VM
parity passing for all `14/14` workloads.

| Evidence | O0 | O1 | Observation |
| --- | ---: | ---: | --- |
| maximum peak live virtual registers | 10 | 10 | O1 never increased pressure |
| workloads with lower pressure | - | 2 | arithmetic and artifact-load variants |
| workloads with unchanged pressure | - | 12 | includes loops, closures, modules, and libraries |
| workloads with higher pressure | - | 0 | no pressure regression in the matrix |
| `library_lfu_cache` bytecode register slots | 22079 | 22222 | +143 slots despite unchanged peak pressure |
| `library_linked_list` bytecode register slots | 22055 | 22198 | +143 slots despite unchanged peak pressure |

The large-library results show why virtual-register numbering and live
pressure must not be conflated: de-SSA can increase emitted slot counts even
when the simultaneously live set remains bounded. That is useful evidence for
a future allocator, but it does not define safe mapping or spill semantics.

## Boundary

This slice changes benchmark reporting and documentation only. It does not
change O0 default behavior, O1 semantics, bytecode opcodes, `.cdbc 0.1`, module
cache identity, Rust VM execution, debugger behavior, or the emitted register
layout. The pressure values are evidence, not a performance threshold, and
this decision does not make O1 the default.
