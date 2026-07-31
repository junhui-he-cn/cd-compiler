# VM-4C-002: structured resource-limit context

Status: implemented on 2026-07-31 on top of `f6e8c9ee`.

## Decision

`RuntimeError` now exposes `resource_limit: Option<usize>`. Resource failures
created by instruction, call-depth, runtime-element, or output-byte checks set
this field to the exact configured limit; ordinary runtime failures,
cancellation, and debugger quit leave it as `None`. `RuntimeErrorKind` remains
the stable discriminator, so hosts can match both the resource class and its
limit without parsing the legacy message.

The existing human-readable message is still generated from the same kind and
limit. Nested call and callback propagation preserves the field together with
the existing location, stack, and source context.

## Compatibility and non-goals

The CLI stderr text, exit codes, profile status lines, trace/debug events,
artifact format, linker errors, and C++/Rust execution behavior are unchanged.
This is an additive field on the `0.1` library error boundary; the library does
not yet promise a versioned JSON schema or a unified error enum.

The field reports the configured rejection limit, not current usage, remaining
budget, host memory, or a retry recommendation. Overflow guards use
`usize::MAX` as their existing diagnostic limit and follow the same field rule.

## Evidence

Rust unit tests assert `resource_limit` for instruction-step, call-depth,
runtime-element, and UTF-8 output budgets, and assert `None` for cancellation.
The library diagnostic fixture asserts `None` for an ordinary runtime error.
Existing CLI/resource/profile tests continue to assert the unchanged text:

```sh
cargo test --manifest-path vm-rs/Cargo.toml
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_resource_budget_tests.py ./build/compiler_design vm-rs
PYTHONDONTWRITEBYTECODE=1 python3 tests/profile_tests.py ./build/compiler_design vm-rs
```

## Migration and next gate

Embedders can migrate from message parsing to `error.kind` plus
`error.resource_limit` while retaining `Display` for user-facing diagnostics.
A JSON/schema slice should be started only after a concrete host consumer and
field set are identified; it must define path/source rendering and versioning
before adding serialization dependencies.
