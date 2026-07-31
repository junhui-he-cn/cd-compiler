# VM-5B-010: borrowed read-only register operands

Status: implemented on 2026-07-31 on top of the cancellation-free instruction
checkpoint fast path.

## Decision

The VM now exposes an internal borrowed register read for operations that only
inspect a value. Numeric unary/binary helpers, ordered comparison, equality,
truthiness branches, and enum tag/field reads use the borrowed path and avoid
cloning the entire register `Value`. Ownership-consuming paths such as calls,
indexing, field mutation, aggregates, stores, and returns continue using the
owned `read_register` path.

The borrowed read still performs the same register bounds check and returns the
same diagnostic on failure. A borrowed value is never retained across a frame
mutation; field extraction clones only the selected field as before.

## Compatibility and non-goals

Arithmetic and comparison results, string concatenation, equality semantics,
enum access, runtime errors, aliasing, trace/debug/profile events, resource
budgets, `.cdbc 0.1`, and public VM/library APIs remain unchanged. This slice
does not change register layout, instruction operands, ownership boundaries,
or aggregate allocation behavior.

## Evidence

The benchmark runner used base commit `8bfc5bbb` with the 009 checkpoint change
held constant in the working tree, manifest revision `bench-2026-07-30-r3`,
three repetitions, and checked stdout/exit contracts:

| Workload | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| `execution_closure` | 0.267173s | 0.255236s | -4.5% |
| `execution_loop` | 0.460723s | 0.437132s | -5.1% |

Cargo tests and the focused benchmark workloads passed after the change. The
full artifact, debugger, profile, and Rust VM golden gates remain required
before delivery of the combined working-tree slices.

