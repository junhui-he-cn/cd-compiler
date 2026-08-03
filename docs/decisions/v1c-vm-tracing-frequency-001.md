# V1C: tracing collection frequency and resource evidence

Status: measured on 2026-08-03. The current top-level safepoint remains the
selected frequency policy; no allocation threshold, incremental scheduler, or
background collector is admitted by this measurement.

## Measurement boundary

The comparison uses the pre-GC V1A implementation at `d234c665` as the
baseline and the non-moving tracing implementation at `3821de81` as the
candidate. Both builds ran on the same host and toolchain:

- Linux WSL2, `x86_64`, 32 CPUs;
- Python 3.12.4;
- CMake 3.28.3;
- rustc 1.94.1 and Cargo 1.94.1;
- benchmark manifest `bench-2026-08-02-r2`;
- manifest SHA-256
  `75ad197d23117be83219ecb563dc14077f33024988f238bbf419bde689856dd0`;
- seven repetitions in this fixed order:
  `artifact_load`, `call_closure`, `collection_helpers`,
  `execution_closure`, `execution_index`, `execution_loop`,
  `library_lfu_cache`, `library_linked_list`, `maps`, `runtime_error`.

The benchmark runner validated every compile/load/runtime sample, expected
stdout/stderr, exit code, output/error digest, and IR/bytecode shape in both
builds. All 10 workloads passed in each run.

## Runtime medians

| Workload | Baseline (s) | Tracing candidate (s) | Delta |
| --- | ---: | ---: | ---: |
| artifact_load | 0.001046 | 0.001066 | +1.91% |
| call_closure | 0.001363 | 0.001355 | -0.59% |
| collection_helpers | 0.001145 | 0.001267 | +10.66% |
| execution_closure | 0.246074 | 0.247868 | +0.73% |
| execution_index | 0.040672 | 0.041022 | +0.86% |
| execution_loop | 0.423656 | 0.429751 | +1.44% |
| library_lfu_cache | 3.171522 | 3.192917 | +0.67% |
| library_linked_list | 0.174732 | 0.179799 | +2.90% |
| maps | 0.001167 | 0.001160 | -0.60% |
| runtime_error | 0.001077 | 0.001056 | -1.95% |

The `collection_helpers` percentage is a sub-millisecond startup-scale
difference, not a stable cross-platform regression signal. The scaled and
library workloads show low-single-digit changes while preserving behavior.
The two library artifact-size differences are caused by different absolute
temporary worktree prefixes embedded in source-backed debug metadata and are
not treated as GC effects.

## Decision

Keep collection at the existing stop-the-world top-level VM safepoint and keep
`Heap::collect_garbage()` as the explicit embedder entry point. The evidence
does not justify collecting on every allocation, adding a byte threshold, or
running collection incrementally or in the background. Such a change would
need a new workload demonstrating retained-cycle pressure during a live,
long-running VM session and a separate resource-budget decision.

No `.cdbc 0.1`, `Value` identity, native registry, debugger, or public API
compatibility boundary changes are required.

## Reproduction

Build baseline and candidate executables, then run the same command sequence
with `--repeat 7` and the workload options listed above:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py \
  <compiler_design> <vm-rs-or-vm-binary> \
  --repeat 7 \
  --workload artifact_load \
  --workload call_closure \
  --workload collection_helpers \
  --workload execution_closure \
  --workload execution_index \
  --workload execution_loop \
  --workload library_lfu_cache \
  --workload library_linked_list \
  --workload maps \
  --workload runtime_error \
  --report <report.json>
```

Generated reports are measurement evidence and remain outside the source
commit.
