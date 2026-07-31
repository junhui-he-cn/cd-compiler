# VM-5B-023: borrow the read-only `Print` operand

Status: implemented on 2026-07-31 on top of commit `ff42db1e`.

## Decision

The bytecode `Print` instruction now borrows its source register through
`read_register_ref` while formatting the output. The previous path cloned the
entire `Value` before converting it to text, which was unnecessary because
`append_output` and the optional trace event only read the value during the
instruction.

The borrow ends before the instruction returns and does not cross output,
trace, or debug callbacks. Trace output still formats the same value and keeps
the existing event order; the output string remains an owned buffer so the
resource check can reject before it is appended.

## Compatibility and non-goals

Primitive, aggregate, function, and Unicode string formatting; UTF-8 output
budget accounting; partial-output suppression; profile counters; trace/debug
events; runtime errors; `.cdbc 0.1`; and CLI behavior remain unchanged. This is
a VM-internal clone reduction. It does not intern strings, retain a borrowed
value across a callback, change output sinks, or alter alias/lifetime rules.

`Len` has its own borrowed boundary in VM-5B-022. Indexing, field access,
aggregate construction, and native calls remain owned paths until their
retention and mutation contracts are audited separately.

## Evidence

Rust tests passed `76 + 3 + 8`. The focused seven-repetition benchmark matrix
passed all six selected workloads and all output/error/exit contracts:
`unicode_strings` median `0.001329s`, `execution_loop` `0.455962s`, and
`execution_closure` `0.264196s`. These startup/run-scale measurements do not
establish a timing threshold; the accepted evidence is removal of the read-only
`Value` clone plus no-regression behavior. The long-output VM-5C corpus and the
full repository gates remain required for delivery.

## Deletion condition

Keep the borrowed operand while `Print` only formats and reports the value. If a
future output sink retains a runtime value or invokes re-entrant VM execution,
revisit the lifetime boundary with an explicit host/output contract before
changing this path.

## Reproduction

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/run_benchmarks.py build/compiler_design vm-rs --repeat 7 --workload unicode_strings --workload execution_loop --workload execution_closure --workload collection_helpers --workload maps --workload runtime_error
```
