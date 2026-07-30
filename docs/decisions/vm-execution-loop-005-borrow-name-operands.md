# VM-5B-005: borrowed hot name operands

Status: implemented on 2026-07-30 on top of the borrowed call-site slice.

## Decision

The VM now exposes an internal borrowed `read_name_ref` view for bytecode name
operands. `LoadVar`, `AssignVar`, `VariantTag`, and struct field access use the
view directly, so their normal paths do not allocate a temporary `String`.
Operations that must retain a name, such as declarations, native dispatch,
constructors, and diagnostic messages, continue to use the owned `read_name`
path.

This keeps the names owned by the immutable `Program`; no per-VM name cache or
artifact change is introduced.

## Compatibility and non-goals

Name-index validation and all variable, field, variant, native, and diagnostic
behavior remain unchanged. The borrowed view is internal and does not alter
`Program`, `.cdbc 0.1`, public library APIs, runtime values, resource limits,
trace/debug/profile behavior, or error locations. This slice does not redesign
the register file or add unsafe dispatch.

## Evidence

The same scaled workloads were run three times before and after the change;
the cache, frame-boundary, and borrowed call-site slices were held constant:

| Workload | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| `execution_closure` | 0.392948s | 0.376456s | -4.2% |
| `execution_loop` | 0.885938s | 0.829240s | -6.4% |

Both focused benchmark runs passed all output, stderr, and exit-status checks.
Runtime-location, field/alias, full debugger/profile, and repository gates are
required before this slice is considered complete.
