# VM-5C-003: long Unicode string output budget boundary

Status: implemented on 2026-07-31 on top of `f6e8c9ee`.

## Decision

Extend the VM-5C long-string corpus with a program that prints the complete
32,768-scalar `é` string. Its output is 65,537 UTF-8 bytes: 65,536 bytes for
the scalar payload and one newline. The direct Rust VM must accept a budget
exactly equal to that byte count and reject a budget one byte smaller.

The accepted and rejection boundaries are fixed as follows:

| Workload | Accepted boundary | Rejection boundary |
| --- | --- | --- |
| long Unicode output | `--max-output-bytes 65537`, complete output | `--max-output-bytes 65536` |

The rejection assertion requires empty stdout, preserving the resource-budget
contract that a failed run does not leak partial program output. The test also
runs the same artifact without an override to retain the default-budget path.
All emit/run measurements continue to use the existing elapsed-time and peak
RSS observation helper.

## Compatibility and non-goals

This slice exercises the existing UTF-8 byte accounting in `append_output` and
does not change `RunConfig`, output formatting, string representation, `.cdbc
0.1`, or CLI diagnostics. It does not introduce a scalar-count output budget,
truncate output, convert RSS into a limit, or change the default 16 MiB budget.
The existing one-scalar resource-budget fixture remains because it checks the
smallest UTF-8 boundary; this capacity case checks scaling and exact equality.

GC, relocating handles, persistent host roots, a binary artifact, and a new
host-memory policy remain outside this decision.

## Evidence

The focused capacity script must pass the long-string length/dump cases, the
full-output default and exact-budget runs, the one-byte-short rejection, and
the existing array, recursion, debug-table, and module-graph cases. Reproduce
the focused gate with:

```sh
cmake -S . -B build
cmake --build build
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_capacity_tests.py ./build/compiler_design vm-rs
ctest --test-dir build --output-on-failure -R vm_capacity
```

## Next boundary

Further string scaling or host-memory policy requires a separate workload and
decision backed by a host consumer. No runtime implementation change is
warranted by this test-only boundary unless the existing byte accounting fails
under a larger accepted workload.
