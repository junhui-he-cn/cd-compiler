# VM-3B-001：deterministic module link report

Status: first report slice implemented on 2026-07-30. The report is additive
to the existing Rust linker and does not change linked `.cdbc 0.1` output or
the default CLI command.

## Decision

`link_modules_with_report` returns `LinkResult { program, report }`. The
compatibility function `link_modules` remains available and returns only the
verified `Program` exactly as before. `LinkReport` records:

- sorted input module identities;
- entry identities in validated `entry_order` order;
- the actual module expansion preorder, with each identity expanded at most
  once;
- input module instruction and dependency counts; and
- linked instruction, function, constant, name, and debug-source counts.

The expansion order is deterministic: entries are visited by contiguous
`entry_order`, and each module's dependency markers are visited in source
order. A diamond graph reports the shared dependency once at its first visit.
The report is available only after verification succeeds. Rejections retain the
existing deterministic `String` diagnostics, including duplicate identity,
missing module, invalid dependency offset/order, and cycle errors; no error
format or CLI exit behavior changes in this slice.

## Boundary and non-goals

The report is an in-memory library result. The `link` CLI does not print or
write it by default, so existing stdout, output artifact bytes, and command
arguments remain compatible. A future opt-in report file or structured linker
error requires a separate schema decision.

This slice does not change dependency resolution, cache policy, `.cdbc` text,
debug-source rebasing, module identity rules, instruction budgets, or linker
performance. It does not duplicate compiler module-cache logic.

## Evidence

Rust linker tests cover a deterministic diamond expansion and the existing
cycle/invalid-input rejection paths. The library integration test checks the
report counts for a linked program and then executes two independent VM
instances. Existing artifact, module artifact, Rust VM golden, debugger, and
CLI compatibility gates remain unchanged.

```sh
cargo test --manifest-path vm-rs/Cargo.toml
```
