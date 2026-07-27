# M5D-DEBUG-001: source tracing, stack inspection, and runtime values

Status: implemented as the first M5D prototype slice.

## Decision

Add `compiler-design-vm trace <program.cdbc>`. The command executes the same
linked program path as `run`, but emits deterministic one-line trace events on
stdout. Events reuse the artifact's existing `debug_sources`,
`debug_locations`, and `debug_ranges` metadata and include:

- function entry, source-location changes, return, exit, and runtime failure;
- the active call stack from the outermost caller to the innermost function;
- sorted current-frame locals using the VM's existing display representation;
- values printed by the program and values returned from functions.

Program output is represented by `output` events, so a trace can be consumed as
one deterministic event stream. Runtime failures still use the existing stderr
diagnostic, including source snippets and call-stack formatting.

## Migration and compatibility

The trace boundary is implemented inside the Rust VM execution path and does
not add a parser, evaluator, source mapper, or `.cdbc` section. The existing
`run` command remains unchanged. Metadata-free artifacts retain function and
event information while reporting explicit unknown source locations.

## Quantitative gate

`tests/debugger_tests.py` compiles a nested-call program, checks deterministic
trace replay, nested source frames, locals, output and return values, and then
checks a runtime failure's trace and existing diagnostic. The test is registered
as the `debugger` CTest and M0D inventory case.

## Out of scope

- interactive breakpoints, stepping, and continue commands;
- persistent debugger sessions;
- register-level inspection and mutation watches;
- changes to the `cdbc 0.1` artifact format.

## Old-path deletion condition

No existing runtime or source-mapping path is deleted. Later debugger slices may
remove temporary event adapters only after all supported debugger consumers use
the shared artifact metadata and VM event boundary.
