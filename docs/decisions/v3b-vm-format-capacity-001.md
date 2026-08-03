# V3B: preallocate canonical artifact formatting output

Status: measured and kept on 2026-08-03. Baseline commit `2afd29db` was
compared with candidate commit `bcc7694a` using the same benchmark sequence.

## Hotspot selection

V3A isolated canonical `dump` from `verify` and showed that the two
multi-megabyte public-library artifacts spent about 33 ms more in `dump` than
in parse/verify. Small artifacts stayed near one millisecond, where process
startup and filesystem noise dominate. A syscall summary for one large dump
also showed only 4.8 ms in traced syscalls, so the next slice targets the
formatter's CPU-side output construction rather than artifact I/O.

The selected mechanism is repeated growth of the outer `String` while
`format_program_sections` appends thousands of formatted instruction and
metadata lines. The expected metric is median `dump_seconds` for the two
large library workloads, with `artifact_load` and `module_link` as small
artifact controls.

## Candidate

`format_program` and module formatting now reserve a conservative capacity hint
derived from instruction count, constants, names, debug-source text, and module
metadata. The hint changes only initial allocation; it does not change
instruction formatting, ordering, escaping, artifact bytes, or the `.cdbc 0.1`
boundary.

## Same-sequence evidence

Both reports used manifest `bench-2026-08-03-r1`, eleven repetitions in this
fixed order: `artifact_load`, `module_link`, `library_lfu_cache`, and
`library_linked_list`. They ran on Linux WSL2 `x86_64` with 32 CPUs, Python
3.12.4, CMake 3.28.3, rustc 1.94.1, and Cargo 1.94.1. Every sample validated
compile, load, dump, runtime output/error/exit, bytecode shape, and canonical
dump digest.

| Workload | Verify baseline -> candidate | Dump baseline -> candidate | Dump delta |
| --- | ---: | ---: | ---: |
| artifact_load | 0.001095 -> 0.001177 s | 0.001056 -> 0.001120 s | +6.06% (startup noise) |
| module_link | 0.001149 -> 0.001200 s | 0.001151 -> 0.001176 s | +2.17% (startup noise) |
| library_lfu_cache | 0.128517 -> 0.122564 s | 0.161170 -> 0.156439 s | -2.94% |
| library_linked_list | 0.124691 -> 0.123110 s | 0.158978 -> 0.155769 s | -2.02% |

The candidate preserved the canonical dump SHA-256 for all four workloads and
preserved runtime stdout, stderr, exit codes, and bytecode shape. The large
artifact improvement is consistent across both library workloads; the small
control changes are sub-0.1 ms and are not treated as regressions.

## Decision and boundary

Keep the capacity hint. It is a local allocation optimization with measurable
benefit on the only formatter-sized workloads, no public API or artifact
change, and an easy rollback. Do not generalize it into a new output buffer
API or a binary artifact format. V3C remains deferred until a capacity case
defines an exact success/rejection boundary or an unbounded host cost.

## Reproduction

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py \
  --repeat 11 \
  --workload artifact_load \
  --workload module_link \
  --workload library_lfu_cache \
  --workload library_linked_list \
  --report build/benchmark-v3b-candidate.json
```

The baseline report is `build/benchmark-v3b-baseline.json`; generated reports
are evidence and are not committed.
