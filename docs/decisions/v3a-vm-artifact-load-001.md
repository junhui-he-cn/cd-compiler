# V3A: separate artifact verification load from canonical dump

Status: measured on 2026-08-03 at commit `fad90190`. The benchmark runner now
has distinct `verify` and `dump` samples; no artifact bytes, parser rules, or
runtime behavior changed.

## Decision

Use the additive `compiler-design-vm verify <artifact>` command as the pure
artifact load boundary for benchmark purposes. It reads the artifact, parses
it, verifies it, and emits no stdout. Keep
`compiler-design-vm dump <artifact>` as the canonical formatting boundary.
The runner records both phases as `load_seconds` and `dump_seconds`, validates
that every dump succeeds without stderr, and checks that repeated canonical
dump digests are identical.

The two samples are independent VM processes. `dump` performs its own read,
parse, and verification before formatting, so the difference between the
medians is diagnostic evidence rather than a pure formatter CPU measurement.
No subtraction-based performance threshold is introduced.

## Evidence

The current manifest (`bench-2026-08-03-r1`, SHA-256
`c0d2a063793750829858627075cc275f54a1df1abb9b8edb3d32ef12fab9eea3`) ran
seven repetitions for all 14 workloads. The same host/toolchain was used for
every sample: Linux WSL2 `x86_64`, 32 CPUs, Python 3.12.4, CMake 3.28.3,
rustc 1.94.1, and Cargo 1.94.1. All 14 workloads passed, including stdout,
stderr, exit-code, artifact-shape, and repeated canonical-dump digest checks.

| Workload | Artifact bytes | Verify median (s) | Dump median (s) | Dump - verify |
| --- | ---: | ---: | ---: | ---: |
| arithmetic | 833 | 0.001174 | 0.001063 | -0.111 ms |
| artifact_load | 833 | 0.001029 | 0.001078 | +0.049 ms |
| call_closure | 3810 | 0.001261 | 0.001200 | -0.061 ms |
| collection_helpers | 1768 | 0.001161 | 0.001190 | +0.029 ms |
| execution_closure | 3605 | 0.001158 | 0.001197 | +0.039 ms |
| execution_index | 3323 | 0.001171 | 0.001233 | +0.062 ms |
| execution_loop | 1573 | 0.001232 | 0.001097 | -0.135 ms |
| library_lfu_cache | 3372550 | 0.125547 | 0.158869 | +33.322 ms |
| library_linked_list | 3369296 | 0.126690 | 0.160441 | +33.751 ms |
| maps | 2344 | 0.001064 | 0.001231 | +0.167 ms |
| module_link | 2877 | 0.001194 | 0.001280 | +0.086 ms |
| native_stdlib_math | 834 | 0.001105 | 0.001079 | -0.026 ms |
| runtime_error | 654 | 0.001376 | 0.001203 | -0.173 ms |
| unicode_strings | 3030 | 0.001214 | 0.001222 | +0.008 ms |

The two multi-megabyte library artifacts are the only workloads where the
dump phase is materially above the verify phase (about 33 ms). Small-artifact
differences are startup and filesystem noise at roughly one millisecond. This
measurement identifies artifact formatting as a possible V3B investigation,
but does not select or implement a formatter optimization.

## Compatibility

`cdbc 0.1`, canonical dump bytes, module linking, resource limits, runtime
diagnostics, and the Rust library API remain unchanged. `verify` accepts both
linked program and module artifacts and applies the same size and verification
limits as `dump`.

## Reproduction

```sh
cargo build --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py \
  --repeat 7 \
  --report build/benchmark-report-v3a.json
```

The JSON report is generated evidence and is intentionally not committed.
