# VM-5B-019: inline ordered-comparison dispatch

Status: implemented on 2026-07-31 on top of commit `8f8ad8d2` and the
VM-4B-002 working tree.

## Decision

The four ordered comparison instructions now pass an internal `Comparison`
enum through the shared comparison helper. Numeric comparisons use the enum's
direct relation match; string comparisons use the same enum to select the
existing Unicode-scalar ordering result. The hot path no longer calls a
function pointer or carries a string opcode name for every numeric comparison.

## Compatibility and non-goals

Number and string ordering, Unicode behavior, type-error text, register bounds
checks, aliasing, resource accounting, profile/debug/trace behavior, and
`.cdbc 0.1` remain unchanged. The enum is VM-internal; this slice does not
specialize bytecode, remove runtime validation, add unsafe dispatch, or change
the comparison semantics for custom operators.

## Evidence

The same VM-4B-002 working tree was built in debug mode for both measurements,
with the `bench-2026-07-30-r3` manifest and seven repetitions. The no-enum
baseline median for `execution_loop` was `0.446904s`; two post-change medians
were `0.419459s` and `0.427508s`. The unrelated `execution_closure` workload
measured `0.254099s` and `0.253886s` after the change, consistent with a control
workload that does not heavily exercise ordered comparison.

All benchmark outputs, stderr, and exit contracts passed. Cargo, artifact,
module artifact, Rust VM golden, debugger, and profile focused gates also
passed; the full repository gate remains required before any delivery commit.
