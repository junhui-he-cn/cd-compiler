# VM future work: host integration and release compatibility deferred

Status: deferred by user direction on 2026-08-03.

## Deferred tracks

The following roadmap work is intentionally paused:

- V2A structured host outcome;
- V2B controlled output/diagnostic sinks;
- V2C external result schema; and
- V4 native/release compatibility and version-matrix work.

The completed V1 lifetime/GC work and V3 artifact benchmark/capacity work are
not reverted. The current CLI, Rust library API, `.cdbc 0.1` format, resource
accounting, diagnostics, debugger/profile behavior, and C++/Rust parity remain
the active compatibility boundary.

## Resume triggers

Resume V2 only when an in-repository host, editor/tool adapter, service, or
other product consumer needs a structured result or controlled I/O. Resume V4
only when a release, native ABI, or compiler/VM version-compatibility need is
demonstrated. At that point, create a fresh decision record naming the
consumer, fields, lifecycle, compatibility promise, and verification matrix
before implementing the API.

Until a trigger appears, development should be limited to regression fixes,
compatibility maintenance, and new evidence-backed work that does not invent a
host or release contract.
