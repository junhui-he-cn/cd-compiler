# V2A: host outcome consumer gate

Status: audited on 2026-08-03; V2A remains queued and no public outcome API was
added.

## Audit result

The repository does not currently contain a real in-process host/product
consumer for the Rust VM library:

- `vm-rs/src/main.rs` is the CLI adapter and intentionally converts typed VM
  results into existing stdout/stderr/exit behavior;
- `vm-rs/tests/library_api.rs` and `vm-rs/tests/cycle_lifetime.rs` are contract
  tests, not a host application with an independent outcome model; and
- the C++ compiler and test runners produce or launch `.cdbc` artifacts rather
  than embedding `VM` in a host workflow.

The audit used repository-wide searches for `compiler_design_vm::`, `VM::`,
`RunConfig`, `parse_artifact_checked`, and `link_modules_checked`, excluding
generated `build/` and `vm-rs/target/` trees. No additional production caller
was found.

## Decision

Do not start V2A yet. Defining a structured outcome without a consumer would
freeze fields and ownership rules speculatively, duplicate the existing typed
`RuntimeError`/`ProfileRun`/trace/debug APIs, and create an unvalidated public
compatibility promise. The CLI remains the compatibility adapter, while the
Rust library types remain authoritative for current tests and embedders.

When a host consumer appears, V2A must first name its required fields and
lifecycle: stdout bytes, typed runtime/resource failure, source/frame context,
and partial profile data. It must then prove that the result can be consumed
without parsing CLI text before V2B sinks or V2C external schemas are
considered.

## Compatibility and verification

No `.cdbc 0.1`, CLI text/exit behavior, Rust library API, resource accounting,
or VM execution semantics changed in this audit. Existing Rust VM and artifact
gates remain the evidence for the current boundary; this decision is
documentation-only and adds no generated report.

## Next trigger

Resume V2A only when an in-repository host application, editor/tool adapter, or
other product consumer needs a structured result. Until then, continue only
with evidence-backed VM work that does not invent a host contract.
