# VM-4A-001：interactive source debugger protocol

Status: first interactive debugger slice implemented on 2026-07-30 on top of
the existing deterministic `trace` boundary. The session is a live execution
hook over a linked `cdbc 0.1` program; it does not pre-execute or replay the
program.

## Decision

The CLI command is:

```text
compiler-design-vm debug <program.cdbc>
```

The command reads one line-oriented command at each pause and writes stable
human-readable records to stdout. A session starts paused before the first
executable instruction with `reason=entry`. Supported commands are:

| Command | Meaning |
| --- | --- |
| `break <path>:<line>` | Break on a one-based source line. Paths are compared after best-effort host canonicalization and otherwise literally. |
| `break-range <path>:<start>-<end>` | Break when an existing source-local half-open debug range overlaps the requested byte range. |
| `continue` / `c` | Resume until a breakpoint or completion. The current matching breakpoint is suppressed until execution leaves its line/range. |
| `step` / `s` | Execute until the next distinct bytecode instruction event, including entry into a nested call. |
| `next` / `n` | Execute until the next distinct instruction at the current or shallower call depth; nested calls are stepped over. |
| `delete <id>` | Remove a breakpoint returned by `break` or `break-range`. |
| `help` | Print the command summary and remain paused. |
| `quit` / `q` | Stop the live VM session without reporting a runtime failure. |

Each pause is one record with the fields `reason`, `function`, `instruction`,
`module`, `location`, `stack`, `locals`, and an optional `range`. The stack is
outermost-to-innermost, locals are sorted and display-formatted, locations use
the artifact's existing one-based line/column metadata, and ranges use the
artifact's source-local byte offsets. Missing metadata is printed explicitly
as `none` or `<unknown>`; no new source mapper is introduced.

The embeddable library exposes the same execution boundary through
`DebugHook`, `DebugPause`, `DebugControl`, `DebugRun`, and `VM::debug`. A hook
is called before each instruction while the VM state and call frames are live.
Returning `Quit` stops the execution cleanly; normal runtime errors retain the
existing `RuntimeError` result and diagnostic path. A runtime failure produces
an `error` pause for each active execution frame from the failing callee back
through its callers, then the final `RuntimeError` is rendered on stderr;
continuing from an error pause does not attempt recovery.

## Compatibility and non-goals

`run`, `trace`, linked/module artifact text, resource budgets, and metadata-free
artifact loading remain unchanged. The debugger accepts linked programs only;
module products must be linked first, using the existing linker. Imported
source metadata works after linking because the existing debug-source rebasing
is reused.

This slice does not support source expression evaluation, register or variable
mutation, watches, hot replacement, persistent sessions, snapshot/rollback,
new artifact fields, or a second source mapper. `step` is intentionally an
instruction event boundary; source-level grouping can be added only with a
separate stop-order decision.

## Migration and deletion condition

The CLI is a thin interactive hook over the library VM. The old trace event
collector remains because trace consumers need a complete non-interactive
execution transcript. It may only be replaced after all trace and debugger
consumers share a tested event contract and metadata-free behavior remains
covered.

## Evidence

`tests/debugger_tests.py` covers deterministic trace compatibility, entry and
line/range breakpoints, step/next, imported modules, function and closure
locals, runtime-error stop order, metadata-free artifacts, quit-session repeatability,
and the existing trace failure path. `vm-rs/tests/library_api.rs` covers the
in-memory hook boundary and VM state isolation. The module-artifact and Rust VM
golden gates remain compatibility checks for linked execution and source
metadata.

```sh
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/debugger_tests.py ./build/compiler_design vm-rs
```
