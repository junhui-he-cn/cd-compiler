# M2B-TYPE-001: aggregate independent module type diagnostics

Status: resolved and implemented.

## Decision

The first type-recovery boundary is the import-aware module scheduler:

- a module body remains stop-first, so a failed semantic state is never reused
  inside that module;
- the scheduler continues to independent modules in dependency-first DFS
  order and reports one located `Type` diagnostic for each failed module;
- a failed module discards its transient scopes, type tables, flow facts, and
  partial module symbols before the next module starts;
- an importer of a failed module, and any transitive importer of a skipped
  module, is skipped without producing a cascade diagnostic; and
- a valid preloaded interface remains a semantic input and does not enter the
  source-body checker.

Direct stdin, single-file, and ordered direct-multi-file inputs retain their
combined-entry path and stop-first type behavior. Lexer, import, compile, and
runtime stop behavior is unchanged, and parser statement-boundary recovery is
unchanged.

Diagnostics retain their existing file path, file-local location, source line,
caret, range, empty stdout, and exit status `1`. The aggregate is raised before
IR, bytecode, or runtime processing.

## Ordering and cascade boundary

The diagnostic order is the order in which the dependency scheduler completes
module visits: dependencies precede importers, graph edge order is preserved,
and module-node order is the deterministic fallback. Failed modules contribute
their first error. Skipped modules contribute none. A successful module that is
not reachable through a failed dependency still completes its semantic check
and produces its normal interface record, even when the overall check later
raises the aggregate.

This is deliberately a module-boundary recovery slice. It does not attempt to
continue after a TypeError within one body, and it does not change the direct
input path.

## Evidence and gate

`module_type_recovery` contains two independent failures, two additional
failure/cascade sites that must remain silent, and one unrelated valid module.
The expected output contains only the first error from each independent failed
module. `frontend_session_tests` additionally checks diagnostic order,
transitive importer suppression, successful unrelated body checking, and
interface production.

The focused gate is:

```sh
cmake --build build --target compiler_design frontend_session_tests
ctest --test-dir build --output-on-failure -R '^frontend_session$'
python3 tests/run_golden_tests.py ./build/compiler_design --case module_type_recovery
python3 tests/verification_inventory.py --write
git diff --check
```

This decision supersedes the M1 baseline's global type stop-first statement
only for the import-aware module scheduler; the direct and per-module
stop-first boundaries remain part of the contract.
