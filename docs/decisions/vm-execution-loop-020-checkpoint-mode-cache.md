# VM-5B-020: cached instruction-checkpoint mode

Status: implemented on 2026-07-31 on top of commit `738cbd49`.

## Decision

`VM::with_config` now derives one internal instruction-checkpoint mode from the
immutable `RunConfig`: bounded or unlimited, with a separate pair of modes for
cooperative cancellation. The hot instruction path matches that cached mode
instead of repeatedly reading the cancellation and instruction-limit options
from the public configuration.

The four modes preserve the existing contracts. A non-cancelled bounded run
checks the limit and uses the existing increment path; a non-cancelled
unlimited run retains checked overflow handling. Both cancellation modes check
the cloned token before the limit and increment, and retain checked overflow
handling. The public `RunConfig`, cancellation token, resource kind, and error
text remain unchanged.

## Compatibility and non-goals

Default budgets, unlimited execution, cancellation timing, native callback
checkpoint accounting, profile/debug/trace behavior, `.cdbc 0.1`, linked/module
artifacts, and the library API remain unchanged. The cached mode is VM-internal;
this slice does not change budget values, add a new scheduler, or alter the
runtime-element/output/call-depth checks.

## Evidence

The baseline was the committed `738cbd49` VM binary and the candidate was built
from the same checkout after this change. The benchmark manifest was
`bench-2026-07-30-r3`; baseline and candidate were run sequentially with eleven
repetitions on the same host, and every output/error/exit contract passed:

| Workload | Baseline median | Candidate median | Observation |
| --- | ---: | ---: | --- |
| `execution_closure` | `0.254038s` | `0.253021s` | no regression |
| `execution_loop` | `0.425305s` | `0.413971s` | `-2.7%` |

Cargo tests (`76 + 3 + 8`) and the five-repetition full benchmark matrix
(`11/11`) passed before documenting the slice. The complete repository gates
remain required before delivery.
