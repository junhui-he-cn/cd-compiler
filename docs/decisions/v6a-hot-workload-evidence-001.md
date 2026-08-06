# V6A: repeatable hot-workload evidence

Status: recorded on 2026-08-06 at commit `f92d9560`. This slice closes the
V6A characterization gate for the current Rust VM workload corpus. It does
not add JIT code, change the `.cdbc 0.1` contract, or establish a wall-clock
performance target.

## Scope

The existing benchmark runner measured four representative O0 workloads from
`tests/benchmark_manifest.json` twice, with five repetitions per run:

- `execution_loop`: a scaled arithmetic/interpreter loop;
- `execution_closure`: repeated closure and function-value calls;
- `execution_index`: repeated array/index operations;
- `collection_helpers`: collection helper calls and callback-free collection
  work.

The local reports are `build/v6a-hot-workload-r1.json` and
`build/v6a-hot-workload-r2.json`. They are generated evidence, not checked-in
baseline artifacts. The runner validates compilation, artifact inspection,
loading, execution output, stderr, and exit status for every repetition.

## Measurement environment

Both reports use benchmark revision `bench-2026-08-03-r1`, commit
`f92d9560e4c85298a4d97534999ba3f0ca351470`, CMake 3.28.3, Rust/Cargo 1.94.1,
Python 3.12.4, and Linux 6.18.33.2-microsoft-standard-WSL2 on x86_64 with 32
logical CPUs. Runtime timing includes Rust VM process startup and artifact
load; compile timing includes compiler process startup and artifact writing.
These measurements are therefore host-specific characterization data, not a
portable throughput claim.

## Evidence

| Workload | Run 1 median | Run 2 median | Run 1 range | Run 2 range | Bytecode | Functions | Registers | Artifact |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `execution_loop` | 439.014 ms | 443.437 ms | 4.4% | 5.5% | 19 | 0 | 12 | 1573 B |
| `execution_closure` | 263.214 ms | 256.985 ms | 2.8% | 1.5% | 41 | 2 | 25 | 3605 B |
| `execution_index` | 41.399 ms | 42.321 ms | 12.3% | 5.3% | 40 | 0 | 28 | 3323 B |
| `collection_helpers` | 1.143 ms | 1.172 ms | 25.5% | 93.0% | 21 | 0 | 16 | 1768 B |

The range columns are `(maximum - minimum) / median` within each five-sample
run. Across both reports:

- all eight workload runs passed;
- bytecode instruction/function/register metrics and artifact sizes were
  identical for each workload;
- observed stdout, stderr, and exit-status digests were identical for each
  workload and repeat;
- the existing V5B cooperative workload/profile tests continue to provide
  deterministic task and profile attribution evidence. The benchmark report
  itself does not pretend to be a per-function VM profile.

The two timing medians moved by `+1.0%` (`execution_loop`), `-2.4%`
(`execution_closure`), `+2.2%` (`execution_index`), and `+2.5%`
(`collection_helpers`) from run 1 to run 2. The small, mixed-direction changes
and the startup-scale spread do not justify a speedup threshold.

## Decision

V6A has enough stable evidence to select candidates for a later runtime
contract, but not enough evidence to implement or claim a JIT benefit:

1. `execution_loop` is the strongest long-running interpreter control
   workload and remains a primary end-to-end candidate.
2. `execution_closure` is the primary function-level candidate because it
   contains two `BytecodeFunction` bodies and repeated closure calls.
3. `execution_index` remains a secondary data-access control workload.
4. `collection_helpers` is retained as a correctness and startup-scale
   control, not as a timing target.

The next slice is V6B: define the JIT runtime contract before generating
machine code. It must preserve interpreter fallback, source/debug and runtime
error mapping, GC roots and safepoints, cancellation and resource checks,
native-call transitions, bounded code-cache policy, and an explicit off switch.
`.cdbc 0.1` remains independent of generated machine code.

## Reproduction

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py \
  ./build/compiler_design vm-rs \
  --repeat 5 \
  --workload execution_loop \
  --workload execution_index \
  --workload execution_closure \
  --workload collection_helpers \
  --report build/v6a-hot-workload-r1.json

PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py \
  ./build/compiler_design vm-rs \
  --repeat 5 \
  --workload execution_loop \
  --workload execution_index \
  --workload execution_closure \
  --workload collection_helpers \
  --report build/v6a-hot-workload-r2.json

cargo test --manifest-path vm-rs/Cargo.toml --test cooperative_workloads
python3 tests/profile_tests.py ./build/compiler_design vm-rs
git diff --check
```

No JIT implementation, native code cache, executable-memory policy, or timing
threshold belongs in this evidence slice.
