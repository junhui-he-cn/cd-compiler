# VM-5C-002: long Unicode string capacity boundary

Status: implemented on 2026-07-31 on top of `f6e8c9ee`.

## Decision

Extend the VM-5C capacity corpus with a 32,768-scalar Unicode string made from
the two-byte UTF-8 scalar `é`. The C++ compiler emits the source-backed `.cdbc`
artifact, and the direct Rust VM must execute and report the scalar length as
`32768`, not the UTF-8 byte length. The same artifact is loaded through `dump`
to retain coverage for constant formatting and debug-source metadata.

The accepted workload and rejection boundary are fixed as follows:

| Workload | Accepted boundary | Rejection boundary |
| --- | --- | --- |
| long Unicode string | 32,768 `é` scalars, `run` prints `32768` | `dump --max-artifact-bytes 1024` |

The test records emit, dump, and run elapsed time and peak RSS through the
existing capacity measurement helper. Those values remain observations rather
than portable performance or host-memory thresholds.

## Compatibility and non-goals

This slice confirms the existing Unicode-scalar `len` contract and the existing
artifact-size budget. It does not change string representation, UTF-8 handling,
output budgets, `.cdbc 0.1`, the Rust VM, or the C++ emitter. The test does not
turn observed artifact size or RSS into a new default limit, add a byte-oriented
string API, or introduce a process-wide allocator policy.

The artifact is intentionally source-backed and includes debug metadata. The
`dump` assertion checks that the Unicode constant and debug-source section
survive canonical loading/formatting without comparing timing or the complete
large output text as a golden file.

## Evidence

The focused capacity script must pass the long-string emit, run, dump, and
artifact-budget cases together with the existing array, recursion, debug-table,
and module-graph cases. The full verification commands remain the repository
gates; the focused reproduction is:

```sh
cmake -S . -B build
cmake --build build
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
ctest --test-dir build --output-on-failure -R vm_capacity
```

## Next boundary

Further string scaling, host-memory policy, allocator measurement, and any
representation change require a separate capacity decision backed by a host
consumer. GC, relocating handles, persistent host roots, and a binary artifact
remain outside this slice.
