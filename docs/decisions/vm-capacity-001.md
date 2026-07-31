# VM-5C-001: capacity and large-module graph corpus

Status: implemented on 2026-07-31 on top of `f6e8c9ee`.

## Decision

The first VM-5C slice is a repeatable capacity gate rather than a new memory
manager or a hard host-memory limit. `tests/vm_capacity_tests.py` creates
temporary artifacts and validates the following boundaries through the C++
emitter and the direct Rust VM binary:

| Workload | Accepted boundary | Rejection boundary |
| --- | --- | --- |
| large array | 4,096 elements, successful run | `--max-elements 1024` |
| deep call | recursion to 64 with `--max-call-depth 128` | `--max-call-depth 8` |
| debug table | 1,400 declarations and 2,800+ main locations | `--max-artifact-bytes 512` |
| long module chain | 12 module products, link and execution | `--max-modules 8` |
| diamond module graph | 4 products, shared dependency expanded once | same linked output contract |

The gate asserts output, product counts, canonical debug-location retention,
and stable resource diagnostics. It also records direct VM load/link/run
elapsed time and peak RSS when the host provides `wait4`; these values are
observations for later workload comparisons, not correctness thresholds.

## Compatibility and non-goals

Default `RunConfig` budgets, `.cdbc 0.1`, module identity and dependency
ordering, aliasing, stdout/stderr separation, and runtime error kinds remain
unchanged. The script does not add a process-wide allocator, convert RSS into
a portable budget, collect cycles, or alter the compiler's module cache
invalidation policy. It does not make large artifacts legal beyond the existing
budget; it fixes representative accepted and rejected sizes in a test corpus.

The capacity gate uses the already built `target/debug/compiler-design-vm`
after one quiet `cargo build`, so its VM measurements do not include repeated
Cargo startup or compilation. Peak RSS is Linux KiB in the current environment
and is explicitly not comparable across allocators or operating systems.

## Evidence

Toolchain for the recorded run: `rustc 1.94.1`, `cargo 1.94.1`, `cmake 3.28.3`,
and Python `3.12.4`. The direct capacity run passed all cases. Representative
observations were:

| Operation | Elapsed | Peak RSS |
| --- | ---: | ---: |
| large-array run | `17.958 ms` | `15,152 KiB` |
| large-debug-table dump | `15.786 ms` | `15,408 KiB` |
| long-chain module link | `3.193 ms` | `16,432 KiB` |
| long-chain linked run | `1.962 ms` | `16,432 KiB` |
| diamond module link | `1.472 ms` | `16,432 KiB` |
| diamond linked run | `1.148 ms` | `16,432 KiB` |

The focused CTest pair `vm_resource_budget` and `vm_capacity` passed `2/2`; the
capacity script itself passed all success and rejection cases.

## Next gate

A later VM-5C slice may add larger workloads or a report file only after these
cases show a reproducible need. It must not turn the observed RSS values into a
default runtime limit without a host/allocator policy decision. GC, relocating
handles, persistent host roots, and a binary artifact remain outside this gate.

## Reproduction

```sh
cmake -S . -B build
cmake --build build
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
ctest --test-dir build --output-on-failure -R 'vm_resource_budget|vm_capacity'
```
