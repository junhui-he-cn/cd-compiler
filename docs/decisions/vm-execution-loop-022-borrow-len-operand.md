# VM-5B-022: borrow the read-only `Len` operand

Status: implemented on 2026-07-31 on top of commit `980d53a6`.

## Decision

The bytecode `Len` instruction now borrows its source register through
`read_register_ref` and passes `&Value` to the read-only `execute_len` helper.
The previous path cloned the register value before checking array, map, range,
or Unicode-scalar string length. No runtime value is retained after the helper
returns, so the borrow is limited to the instruction and does not change alias
or lifetime behavior.

The direct helper tests were updated to use borrowed values. Existing C++
emission, Rust artifact parsing, and Unicode `len` coverage continue to exercise
the bytecode path.

## Compatibility and non-goals

Array/map/range/string length, type errors, register bounds errors, resource
checkpoints, profile counters, trace/debug events, `.cdbc 0.1`, and CLI output
remain unchanged. This is a VM-internal ownership reduction; it does not change
the public library API, add a string cache, alter aggregate storage, or borrow
any value across a native call.

The slice is intentionally limited to `Len`. `Print`, indexing, field access,
and aggregate construction still use owned values because their helpers may
retain or mutate data and require separate alias/lifetime audits.

## Evidence

Rust tests passed `76 + 3 + 8`. The five-sample benchmark matrix passed all
`11/11` workloads and all output/error/exit contracts. Relative to the
pre-change `980d53a6` run, the candidate median for `unicode_strings` was
`0.001261s` versus `0.001615s`; the execution-loop and closure controls stayed
within ordinary run-to-run variance, so no timing threshold is established.
The accepted evidence is the removed read-only `Value` clone plus no-regression
behavior across the full benchmark matrix.

## Deletion condition

Keep the borrowed helper boundary while `execute_len` remains read-only. Revert
to an owned operand only if a future implementation needs to retain or mutate
the value during length computation; that change must restore an explicit
ownership test and recheck aggregate aliasing.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py build/compiler_design vm-rs --repeat 5
```
