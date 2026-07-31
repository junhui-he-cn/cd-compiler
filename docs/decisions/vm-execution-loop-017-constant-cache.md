# VM-5B-017: per-VM decoded constant cache

Status: implemented on 2026-07-31 on top of commit `0295380c`.

## Decision

Each `VM` instance now owns a cache parallel to `Program::constants`. The first
successful execution of a constant decodes it into the existing runtime
`Value`; later `Constant` instructions clone that cached value instead of
parsing the artifact representation again. Invalid indexes and invalid number
literals still return their existing runtime errors and are not cached.

The cache is VM-local and contains only the already supported primitive
constant values. It does not mutate `Program`, change the `.cdbc 0.1` format, or
share mutable aggregate storage between VM instances. String results still
clone their immutable string payload when returned to a register.

## Compatibility and non-goals

Constant value formatting, register ownership, instruction budgets, profile
counters, trace/debug events, runtime diagnostics, linked/module artifacts, and
the C++/Rust boundary remain unchanged. This slice does not intern strings,
change the artifact constant representation, or cache compiled instructions.

## Evidence

The benchmark runner used base commit `0295380c`, manifest revision
`bench-2026-07-30-r3`, and checked stdout/stderr/exit contracts. The direct
three-repetition comparison was:

| Workload | Before median | After median | Observation |
| --- | ---: | ---: | --- |
| `collection_helpers` | 0.001189s | 0.001224s | startup-scale noise |
| `execution_closure` | 0.256997s | 0.245953s | -4.3% |
| `execution_loop` | 0.441459s | 0.415556s | -5.9% |
| `native_stdlib_math` | 0.001292s | 0.001212s | startup-scale noise |

A seven-repetition follow-up for the scaled workloads measured `0.246968s`
for `execution_closure` and `0.403762s` for `execution_loop`; the result is
treated as workload evidence, not a correctness threshold.

Focused verification passed the constant-cache regression and the complete
Cargo test set (`74 + 3 + 8` tests). The artifact, module, Rust golden,
debugger, and profile gates are recorded with the subsequent VM-5B-018 slice.
